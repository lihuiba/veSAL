// This example demonstrates how to use the EPOLL poll mode for event-driven
// compression/decompression. Instead of busy-polling with Poll(), the user
// integrates the QAT file descriptor into their own epoll loop and calls
// Poll() only when the fd is readable (i.e., hardware has completed requests).
//
// Prerequisites: QAT driver must be configured in EPOLL mode (not the default
// Polling mode).

#include <sys/epoll.h>
#include <cstring>
#include <memory>

#include "gflags/gflags.h"
#include "vesal/codec.h"
#include "vesal/log_setting.h"
#include "vesal/memory_pool.h"
#include "vesal/status.h"
#include "vesal/vesal.h"

// Wait for up to `expected` requests to complete via epoll.
// Assumes qat_fd > 0 (EPOLL mode is active).
// Falls back to busy-polling if epoll API calls fail.
static int WaitForCompletion(vesal::CodecChannel* channel,
                             vesal::CodecResult* results,
                             int expected,
                             int qat_fd) {
    // Busy-poll from the given offset until all expected results are collected.
    auto do_reap = [&](int done, int timeout) {
        while (done < expected) {
            ssize_t n = channel->Poll(results + done, expected - done, timeout);
            if (n > 0) done += n;
        }
        return done;
    };

    int epfd = epoll_create1(0);
    if (epfd < 0) {
        VESAL_LOG(WARN) << "epoll_create1 failed, falling back to busy-polling, errno=" << errno;
        return do_reap(0, -1);
    }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = qat_fd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, qat_fd, &ev) < 0) {
        VESAL_LOG(WARN) << "epoll_ctl ADD failed, falling back to busy-polling, errno=" << errno;
        close(epfd);
        return do_reap(0, -1);
    }

    int completed = 0;
    while (completed < expected) {
        struct epoll_event events[16];
        int nfds = epoll_wait(epfd, events, 16, -1);
        if (nfds < 0) {
            VESAL_LOG(WARN) << "epoll_wait failed, falling back to busy-polling, errno=" << errno;
            close(epfd);
            return do_reap(completed, -1);
        }
        for (int i = 0; i < nfds; i++) {
            if (events[i].data.fd == qat_fd)
                completed = do_reap(completed, 0)
        }
    }
    close(epfd);
    return completed;
}

int main(int argc, char** argv) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    FLAGS_vesal_log_console_output = true;

    // 1. Init veSAL
    VESAL_LOG(INFO) << "Initializing veSAL";
    vesal::InitOptions init_option;
    VESAL_CHECK(vesal::Init(init_option)) << "Init veSAL failed";

    // 2. Create codec channel in EPOLL mode
    VESAL_LOG(INFO) << "Creating codec channel with EPOLL poll mode";
    vesal::CodecChannelOption chan_option;
    chan_option.poll_mode = vesal::CodecPollMode::kEpoll;
    // Dedicated 模式则是一对一：每个 channel 独占一个 QAT 引擎，没有队列，用户自己调 Poll()。
    chan_option.mode = vesal::ChannelMode::kDedicated;
    auto res = vesal::CodecChannel::CreateCodecChannel(chan_option);
    VESAL_CHECK(res.first.ok()) << "Create codec channel failed: " << res.first;
    std::unique_ptr<vesal::CodecChannel> channel = std::move(res.second);

    // 3. Get QAT file descriptor
    int qat_fd = channel->GetFileDescriptor();
    VESAL_CHECK(qat_fd > 0) << "Failed to get QAT fd, EPOLL mode not available";
    VESAL_LOG(INFO) << "Got QAT fd=" << qat_fd << " for epoll integration";

    // 4. Prepare test data
    const unsigned int kInputLen = 4096;
    const unsigned int kOutputLen = 8192;
    const int kRequestNum = 10;
    vesal::CodecResult results[kRequestNum];
    unsigned char* inputs[kRequestNum];
    unsigned char* outputs[kRequestNum];
    for (int i = 0; i < kRequestNum; i++) {
        inputs[i] = (unsigned char*)vesal::MemoryPool::GetInstance()->Allocate(kInputLen);
        VESAL_CHECK(inputs[i]) << "MemoryPool allocate failed";
        outputs[i] = (unsigned char*)vesal::MemoryPool::GetInstance()->Allocate(kOutputLen);
        VESAL_CHECK(outputs[i]) << "MemoryPool allocate failed";
        memset(inputs[i], 'a' + i, kInputLen);
    }

    // 5. Submit async compress requests
    VESAL_LOG(INFO) << "Submitting " << kRequestNum << " compress requests";
    for (int i = 0; i < kRequestNum; i++) {
        auto r = channel->CompressAsync(inputs[i], kInputLen, outputs[i], kOutputLen, nullptr);
        VESAL_CHECK(vesal::IsOk(r)) << "CompressAsync failed: " << r;
    }

    // 6. Wait for compress completion via epoll
    int completed = WaitForCompletion(channel.get(), results, kRequestNum, qat_fd);

    // 7. Verify results
    VESAL_LOG(INFO) << "Verifying " << completed << " results";
    for (int i = 0; i < completed; i++) {
        VESAL_CHECK(vesal::IsOk(results[i].status))
            << "Compress failed: " << results[i].status;
        VESAL_CHECK(results[i].consumed == kInputLen)
            << "Consumed mismatch: expected " << kInputLen << ", got " << results[i].consumed;
        VESAL_LOG(INFO) << "Compress OK, consumed=" << results[i].consumed
                        << ", produced=" << results[i].produced;
    }

    // 8. Decompress with epoll (same pattern)
    VESAL_LOG(INFO) << "Submitting " << kRequestNum << " decompress requests";
    vesal::CodecResult decomp_results[kRequestNum];
    for (int i = 0; i < kRequestNum; i++) {
        // Use compressed output as decompress input; allocate new output buffer
        unsigned char* decomp_buf =
            (unsigned char*)vesal::MemoryPool::GetInstance()->Allocate(kInputLen);
        VESAL_CHECK(decomp_buf) << "MemoryPool allocate failed";
        auto r = channel->DecompressAsync(outputs[i], results[i].produced, decomp_buf,
                                           kInputLen, decomp_buf);
        VESAL_CHECK(vesal::IsOk(r)) << "DecompressAsync failed: " << r;
    }

    completed = WaitForCompletion(channel.get(), decomp_results, kRequestNum, qat_fd);

    VESAL_LOG(INFO) << "Verifying " << completed << " decompress results";
    for (int i = 0; i < completed; i++) {
        VESAL_CHECK(vesal::IsOk(decomp_results[i].status))
            << "Decompress failed: " << decomp_results[i].status;
        VESAL_CHECK(decomp_results[i].produced == kInputLen)
            << "Produced mismatch: expected " << kInputLen
            << ", got " << decomp_results[i].produced;
        VESAL_LOG(INFO) << "Decompress OK, consumed=" << decomp_results[i].consumed
                        << ", produced=" << decomp_results[i].produced;
    }

    // 9. Cleanup
    VESAL_LOG(INFO) << "Cleaning up";
    for (int i = 0; i < kRequestNum; i++) {
        vesal::MemoryPool::GetInstance()->Deallocate(inputs[i]);
        vesal::MemoryPool::GetInstance()->Deallocate(outputs[i]);
    }
    auto r = channel->Close();
    VESAL_CHECK(r.ok()) << "Close channel failed: " << r;
    vesal::Uninit();
    return 0;
}
