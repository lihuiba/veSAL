/*
 * Copyright (c) 2025 ByteDance Inc.
 *
 * This file is part of veSAL.
 *
 * veSAL is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * veSAL is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with veSAL. If not, see <https://www.gnu.org/licenses/>.
 */

// This example demonstrates the shared channel with EPOLL poll mode.
// When poll_mode is kEpoll and mode is kShared, the internal poller thread
// uses epoll_wait() instead of usleep() for efficient event-driven polling.
// The user API remains the same as the regular shared channel — no epoll
// management is needed on the user side.
//
// Prerequisites: QAT driver must be configured in EPOLL mode (not the default
// Polling mode).

#include <cstring>
#include <memory>

#include "gflags/gflags.h"
#include "vesal/codec.h"
#include "vesal/log_setting.h"
#include "vesal/memory_pool.h"
#include "vesal/status.h"
#include "vesal/vesal.h"

int main(int argc, char** argv) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    FLAGS_vesal_log_console_output = true;

    // 1. Init veSAL
    VESAL_LOG(INFO) << "Initializing veSAL";
    vesal::InitOptions init_option;
    VESAL_CHECK(vesal::Init(init_option)) << "Init veSAL failed";

    // 2. Create shared codec channel with EPOLL poll mode
    //    The internal poller thread will use epoll_wait() instead of busy-polling,
    //    reducing CPU usage while maintaining low latency.
    VESAL_LOG(INFO) << "Creating shared codec channel with EPOLL poll mode";
    vesal::CodecChannelOption chan_option;
    chan_option.poll_mode = vesal::CodecPollMode::kEpoll;
    // "Shared" 指的是多个 QatCodecSharedChannel 共享同一个 QatPollerWorker（即同一个 QAT 硬件实例）
    chan_option.mode = vesal::ChannelMode::kShared; 
    auto res = vesal::CodecChannel::CreateCodecChannel(chan_option);
    VESAL_CHECK(res.first.ok()) << "Create codec channel failed: " << res.first;
    std::unique_ptr<vesal::CodecChannel> channel = std::move(res.second);

    // 3. Prepare test data
    const unsigned int kInputLen = 4096;
    const unsigned int kOutputLen = 8192;
    const int kRequestNum = 10;
    std::vector<unsigned char*> inputs(kRequestNum), outputs(kRequestNum);
    for (int i = 0; i < kRequestNum; i++) {
        inputs[i] = (unsigned char*)vesal::MemoryPool::GetInstance()->Allocate(kInputLen);
        VESAL_CHECK(inputs[i]) << "MemoryPool allocate failed";
        outputs[i] = (unsigned char*)vesal::MemoryPool::GetInstance()->Allocate(kOutputLen);
        VESAL_CHECK(outputs[i]) << "MemoryPool allocate failed";
        memset(inputs[i], 'a' + i, kInputLen);
    }

    // 4. Submit async compress requests
    VESAL_LOG(INFO) << "Submitting " << kRequestNum << " compress requests";
    for (int i = 0; i < kRequestNum; i++) {
        auto r = channel->CompressAsync(inputs[i], kInputLen, outputs[i], kOutputLen, nullptr);
        VESAL_CHECK(vesal::IsOk(r)) << "CompressAsync failed: " << r;
    }

    // 5. Poll results — same API as non-EPOLL shared channel.
    //    Internally, the poller thread is woken by epoll_wait() when QAT
    //    hardware completes requests, so results are harvested with lower
    //    CPU overhead than busy-polling.
    VESAL_LOG(INFO) << "Polling compress results";
    vesal::CodecResult comp_results[kRequestNum];
    for (int completed = 0; completed < kRequestNum;) {
        ssize_t n = channel->Poll(comp_results + completed, kRequestNum - completed, -1);
        VESAL_CHECK(n > 0) << "Poll failed";
        completed += n;
    }

    // 6. Verify compress results
    for (int i = 0; i < kRequestNum; i++) {
        VESAL_CHECK(vesal::IsOk(comp_results[i].status))
            << "Compress failed: " << comp_results[i].status;
        VESAL_CHECK(comp_results[i].consumed == kInputLen)
            << "Consumed mismatch: expected " << kInputLen
            << ", got " << comp_results[i].consumed;
        VESAL_LOG(INFO) << "Compress OK, consumed=" << comp_results[i].consumed
                        << ", produced=" << comp_results[i].produced;
    }

    // 7. Decompress (same pattern)
    VESAL_LOG(INFO) << "Submitting " << kRequestNum << " decompress requests";
    vesal::CodecResult decomp_results[kRequestNum];
    for (int i = 0; i < kRequestNum; i++) {
        unsigned char* decomp_buf =
            (unsigned char*)vesal::MemoryPool::GetInstance()->Allocate(kInputLen);
        VESAL_CHECK(decomp_buf) << "MemoryPool allocate failed";
        auto r = channel->DecompressAsync(outputs[i], comp_results[i].produced, decomp_buf,
                                           kInputLen, decomp_buf);
        VESAL_CHECK(vesal::IsOk(r)) << "DecompressAsync failed: " << r;
    }

    for (int completed = 0; completed < kRequestNum;) {
        ssize_t n = channel->Poll(decomp_results + completed, kRequestNum - completed, -1);
        VESAL_CHECK(n > 0) << "Poll failed";
        completed += n;
    }

    for (int i = 0; i < kRequestNum; i++) {
        VESAL_CHECK(vesal::IsOk(decomp_results[i].status))
            << "Decompress failed: " << decomp_results[i].status;
        VESAL_CHECK(decomp_results[i].produced == kInputLen)
            << "Produced mismatch: expected " << kInputLen
            << ", got " << decomp_results[i].produced;
        VESAL_LOG(INFO) << "Decompress OK, consumed=" << decomp_results[i].consumed
                        << ", produced=" << decomp_results[i].produced;
    }

    // 8. Cleanup
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
