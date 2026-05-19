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

#include "codec/qat/qat_poller.h"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <cstring>

#include "common/metrics_internal.h"
#include "common/scheduler.h"
#include "gflags/gflags.h"
#include "vesal/vesal.h"

DEFINE_uint32(
    vesal_codec_qat_shared_mode_poller_op_process_num,
    16,
    "How many PollerOp can be handled by one poller in one polling. We don't want to "
    "create/destruct too many workers in a row and cause starvation on IO requests. If "
    "the number is bigger than 512, 512 will be used. Support modifying this flag in runtime.");
static bool VesalQatPollerPollerOpProcessNumValidator(const char* name, uint32_t value) {
    return value <= vesal::qat::kMaxPollerOpProcessNum;
}
DEFINE_validator(vesal_codec_qat_shared_mode_poller_op_process_num,
                 &VesalQatPollerPollerOpProcessNumValidator);

namespace vesal {
namespace qat {

// A poller can hold at most kMaxWorkerNumPerPoller workers.
const size_t kMaxWorkerNumPerPoller = 128;

// TODO(sjj): Use pool to reduce memory allocation.
inline PollerTask* NewPollerTask() {
    return new PollerTask();
}

inline void FreePollerTask(PollerTask* task) {
    delete task;
}

void QatPoller::Start() {
    if (running_) {
        return;
    }
    InitEpoll();
    poller_thread_ = std::thread(&QatPoller::Run, this);
    VESAL_LOG(INFO) << "QatPoller started, poller_id: " << poller_id_;
    Tag poller_id_tag = {"poller_id", std::to_string(poller_id_)};
    metric_poller_poller_worker_num_ =
        g_metric_registry->RegisterGauge("qat_poller_poller_worker_num", {poller_id_tag});
    metric_poller_channel_num_ =
        g_metric_registry->RegisterGauge("qat_poller_channel_num", {poller_id_tag});
    periodic_task_id_ = g_periodic_scheduler.AddPeriodicTask(
        [&]() {
            metric_poller_poller_worker_num_->Set(GetWorkerNum());
            metric_poller_channel_num_->Set(GetRefCnt());
        },
        std::chrono::milliseconds(1000));
    running_ = true;
}

void QatPoller::Shutdown() {
    if (!running_) {
        return;
    }
    g_periodic_scheduler.CompleteTask(periodic_task_id_);
    auto* task = NewPollerTask();
    task->op = PollerOp::kShutdown;
    poller_tasks_.Push(task);
    poller_thread_.join();
    FreePollerTask(task);
    running_ = false;
}

QatPollerWorker* QatPoller::GetOrNewWorker(const CodecChannelOption& option) {
    if (!running_) {
        return nullptr;
    }
    auto* task = NewPollerTask();
    task->op = PollerOp::kGet;
    task->option = option;
    poller_tasks_.Push(task);
    // Wake up the poller thread if it's blocked in epoll_wait
    if (wakeup_fd_ >= 0) {
        uint64_t notify_val = 1;
        write(wakeup_fd_, &notify_val, sizeof(notify_val));
    }
    auto* r = task->get_r.get_future().get();
    if (r == nullptr) {
        VESAL_LOG(ERROR) << "Failed to Get or New worker, option=" << option
                         << ", poller_id: " << poller_id_;
    } else {
        VESAL_LOG(INFO) << "Get worker, option=" << option << ", worker=" << r
                        << ", poller_id: " << poller_id_;
    }
    FreePollerTask(task);
    return r;
}

bool QatPoller::ReturnWorker(QatPollerWorker* worker) {
    if (!running_) {
        return false;
    }
    auto* task = NewPollerTask();
    task->op = PollerOp::kReturn;
    task->worker = worker;
    CodecChannelOption option = worker->GetOption();
    poller_tasks_.Push(task);
    // Wake up the poller thread if it's blocked in epoll_wait
    if (wakeup_fd_ >= 0) {
        uint64_t notify_val = 1;
        write(wakeup_fd_, &notify_val, sizeof(notify_val));
    }
    auto r = task->return_r.get_future().get();
    VESAL_LOG(INFO) << "Returned worker, option=" << option << ", poller_id: " << poller_id_
                    << ", worker removed: " << r;
    FreePollerTask(task);
    return r;
}

void QatPoller::Run() {
    pthread_setname_np(pthread_self(), ("qat_poller_" + std::to_string(poller_id_)).c_str());
    static constexpr int kMaxEpollEvents = 64;
    struct epoll_event events[kMaxEpollEvents];
    while (true) {
        size_t num = DrainTasks(FLAGS_vesal_codec_qat_shared_mode_poller_op_process_num);
        for (size_t i = 0; i < num; ++i) {
            auto* task = task_buffer_[i];
            switch (task->op) {
            case PollerOp::kGet:
                task->get_r.set_value(GetOrNewWorkerInternal(task->option));
                break;
            case PollerOp::kReturn:
                task->return_r.set_value(ReturnWorkerInternal(task->worker));
                break;
            case PollerOp::kShutdown:
                ShutdownInternal();
                return;
            default:
                VESAL_DCHECK(false)
                    << "Unknown poller op=" << static_cast<int>(task->op) << ", ignored.";
            }
        }
        for (auto& kv : workers_) {
            auto* worker = kv.second->worker.get();
            worker->LoopOnce(kMaxSubmitNum, kMaxPollingNum);
        }
        // Update epoll set after loop (detects HA fd changes)
        UpdateEpollSetAfterLoop();

        if (has_epoll_workers_ && epoll_fd_ >= 0) {
            // Use epoll_wait for efficient notification instead of busy-polling
            int nfds = epoll_wait(epoll_fd_, events, kMaxEpollEvents, 1000);  // 1s fallback
            if (nfds < 0 && errno != EINTR) {
                VESAL_LOG(ERROR) << "epoll_wait failed, errno=" << errno;
            }
            // Drain wakeup events so eventfd doesn't accumulate
            if (wakeup_fd_ >= 0) {
                uint64_t val;
                while (read(wakeup_fd_, &val, sizeof(val)) == sizeof(val)) { /* drain all */ }
            }
        } else {
            // Original behavior: optional sleep
            if (FLAGS_vesal_codec_qat_shared_mode_poller_sleep_time_us > 0) {
                usleep(FLAGS_vesal_codec_qat_shared_mode_poller_sleep_time_us);
            }
        }
    }
}

QatPollerWorker* QatPoller::GetOrNewWorkerInternal(const CodecChannelOption& option) {
    // No need lock here because we are in QatPoller now.
    if (GetRefCnt() >= kMaxWorkerNumPerPoller) {
        return nullptr;
    }
    auto it = workers_.find(option);
    if (it != workers_.end()) {
        it->second->ref_cnt++;
        return it->second->worker.get();
    }
    // Need a new worker.
    auto worker_info = std::make_unique<QatPollerWorkerInfo>();
    worker_info->ref_cnt = 1;
    worker_info->worker = std::make_unique<QatPollerWorker>();
    auto create_r = worker_info->worker->Init(option, poller_id_);
    if (!IsOk(create_r)) {
        VESAL_LOG(ERROR) << "Failed to create codec channel, error=" << create_r
                         << ", poller_id: " << poller_id_;
        return nullptr;
    }
    auto w = worker_info->worker.get();
    int fd = worker_info->known_fd = worker_info->worker->GetFileDescriptor();
    workers_[option] = std::move(worker_info);
    if (fd >= 0) {
        AddWorkerFdToEpoll(fd);
        has_epoll_workers_ = true;
    }
    return w;
}

bool QatPoller::ReturnWorkerInternal(QatPollerWorker* worker) {
    if (worker == nullptr) {
        return false;
    }
    const CodecChannelOption& option = worker->GetOption();
    // Note this is accessed by poller per se so no need lock.
    auto it = workers_.find(option);
    VESAL_DCHECK(it != workers_.end())
        << "Try to retrun a non-existing worker. worker: " << worker
        << ", poller id: " << poller_id_ << ", entries_.size(): " << workers_.size();
    if (it == workers_.end() || it->second->worker.get() != worker) {
        VESAL_LOG(WARN) << "Tried to return a non-existing worker. worker: " << worker
                        << ", option: " << worker->GetOption() << ", poller id: " << poller_id_
                        << ", entries_.size(): " << workers_.size();
        return false;
    }
    it->second->ref_cnt--;
    if (it->second->ref_cnt == 0) {
        // Remove fd from epoll set if present
        int fd = it->second->known_fd;
        if (fd >= 0 && epoll_fd_ >= 0) {
            RemoveWorkerFdFromEpoll(fd);
        }
        it->second->worker->Uninit();
        VESAL_LOG(INFO) << "Removed worker, option: " << option << ", poller id: " << poller_id_;
        workers_.erase(it);
        // Recheck if we still have epoll workers
        has_epoll_workers_ = false;
        for (auto& kv : workers_) {
            if (kv.second->known_fd >= 0) {
                has_epoll_workers_ = true;
                break;
            }
        }
        return true;
    }
    return false;
}

void QatPoller::ShutdownInternal() {
    for (auto& kv : workers_) {
        kv.second->worker->Uninit();
    }
    // Uninit() ensures all inflight requests are finished. So we can safely exit.
    workers_.clear();
    has_epoll_workers_ = false;
    if (epoll_fd_ >= 0) {
        close(epoll_fd_);
        epoll_fd_ = -1;
    }
    if (wakeup_fd_ >= 0) {
        close(wakeup_fd_);
        wakeup_fd_ = -1;
    }
}

size_t QatPoller::DrainTasks(size_t task_num) {
    task_num = std::min(
        task_num, static_cast<size_t>(FLAGS_vesal_codec_qat_shared_mode_poller_op_process_num));
    size_t num = 0;
    PollerTask* task = nullptr;
    while (num < task_num && poller_tasks_.Pop(&task)) {
        task_buffer_[num++] = task;
    }
    return num;
}

void QatPoller::AddWorkerFdToEpoll(int fd) {
    if (fd < 0 || epoll_fd_ < 0) return;
    struct epoll_event ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
        VESAL_LOG(ERROR) << "Failed to add worker fd=" << fd << " to epoll, errno=" << errno;
    } else {
        VESAL_LOG(INFO) << "Added worker fd=" << fd << " to poller epoll, poller_id=" << poller_id_;
    }
}

void QatPoller::RemoveWorkerFdFromEpoll(int fd) {
    if (fd < 0 || epoll_fd_ < 0) return;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr) < 0) {
        VESAL_LOG(WARN) << "Failed to remove worker fd=" << fd << " from epoll, errno=" << errno;
    } else {
        VESAL_LOG(INFO) << "Removed worker fd=" << fd << " from poller epoll, poller_id=" << poller_id_;
    }
}

void QatPoller::UpdateEpollSetAfterLoop() {
    // Detect HA fd changes: if a worker's fd changed after reinit, update the epoll set
    for (auto& kv : workers_) {
        int current_fd = kv.second->worker->GetFileDescriptor();
        int known_fd = kv.second->known_fd;
        if (current_fd != known_fd) {
            VESAL_LOG(INFO) << "Worker fd changed from " << known_fd << " to " << current_fd
                            << " (likely HA reinit), poller_id=" << poller_id_;
            if (known_fd >= 0) {
                RemoveWorkerFdFromEpoll(known_fd);
            }
            if (current_fd >= 0) {
                AddWorkerFdToEpoll(current_fd);
            }
            kv.second->known_fd = current_fd;
        }
    }
}

void QatPoller::InitEpoll() {
    epoll_fd_ = -1;
    wakeup_fd_ = -1;
    has_epoll_workers_ = false;

    // Create epoll fd for EPOLL-mode worker notification
    epoll_fd_ = epoll_create1(0);
    if (epoll_fd_ < 0) {
        VESAL_LOG(ERROR) << "Failed to create epoll fd, errno=" << errno
                         << ", poller will fall back to usleep";
        return;
    }

    // Create wakeup fd for task notification (unblocks epoll_wait when new tasks arrive)
    wakeup_fd_ = eventfd(0, EFD_NONBLOCK);
    if (wakeup_fd_ < 0) {
        VESAL_LOG(ERROR) << "Failed to create wakeup eventfd, errno=" << errno
                         << ", closing epoll_fd and falling back to usleep";
        close(epoll_fd_);
        epoll_fd_ = -1;
        return;
    }

    // Register wakeup_fd into epoll set
    struct epoll_event ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.fd = wakeup_fd_;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wakeup_fd_, &ev) < 0) {
        VESAL_LOG(ERROR) << "Failed to add wakeup_fd to epoll, errno=" << errno
                         << ", closing fds and falling back to usleep";
        close(wakeup_fd_);
        wakeup_fd_ = -1;
        close(epoll_fd_);
        epoll_fd_ = -1;
        return;
    }

    VESAL_LOG(INFO) << "Poller epoll initialized, epoll_fd=" << epoll_fd_
                    << ", wakeup_fd=" << wakeup_fd_ << ", poller_id=" << poller_id_;
}

}  // namespace qat
}  // namespace vesal
