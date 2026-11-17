#pragma once

#include "native_log.hpp"

#include <condition_variable>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <pthread.h>
#include <type_traits>
#include <utility>

namespace linrtx {

/**
 * Dedicated persistent OS worker thread with an explicit 8 MiB native stack.
 *
 * HotSpot JVM defaults client/render threads to a small stack (-Xss1m) with
 * guard pages. NVIDIA's libnvidia-ngx.so.1 / CUDA / feature runtimes can
 * overflow or collide with those guard pages on init and feature calls;
 * running NGX/Vulkan control work here avoids JVM thread-stack faults while
 * keeping all OpenGL calls on Minecraft's Render thread.
 */
class NgxWorker {
public:
    static constexpr size_t kStackSizeBytes = 8u * 1024u * 1024u;

    NgxWorker() = default;
    ~NgxWorker() { shutdown(); }

    NgxWorker(const NgxWorker&) = delete;
    NgxWorker& operator=(const NgxWorker&) = delete;

    template <typename Fn>
    auto run(Fn&& fn) -> std::invoke_result_t<Fn> {
        using Ret = std::invoke_result_t<Fn>;
        static_assert(!std::is_reference_v<Ret>, "NgxWorker::run does not support reference returns");

        ensureStarted();

        if (!started_ || pthread_equal(pthread_self(), thread_)) {
            return std::forward<Fn>(fn)();
        }

        Task task;
        std::exception_ptr ex;
        bool done = false;

        if constexpr (std::is_void_v<Ret>) {
            task.fn = [&]() {
                try {
                    fn();
                } catch (...) {
                    ex = std::current_exception();
                }
            };
            task.done = &done;
            submit(&task);
            if (ex) std::rethrow_exception(ex);
            return;
        } else {
            std::optional<Ret> result;
            task.fn = [&]() {
                try {
                    result.emplace(fn());
                } catch (...) {
                    ex = std::current_exception();
                }
            };
            task.done = &done;
            submit(&task);
            if (ex) std::rethrow_exception(ex);
            return std::move(*result);
        }
    }

    void shutdown() {
        pthread_t toJoin{};
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (!started_ || pthread_equal(pthread_self(), thread_)) return;
            stopRequested_ = true;
            toJoin = thread_;
            cv_.notify_all();
        }
        pthread_join(toJoin, nullptr);
        std::lock_guard<std::mutex> lock(mu_);
        started_ = false;
        stopRequested_ = false;
    }

private:
    struct Task {
        std::function<void()> fn;
        bool* done = nullptr;
        Task* next = nullptr;
    };

    void ensureStarted() {
        std::lock_guard<std::mutex> lock(mu_);
        if (started_) return;

        pthread_attr_t attr{};
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, kStackSizeBytes);
        size_t requestedStack = 0;
        pthread_attr_getstacksize(&attr, &requestedStack);

        stopRequested_ = false;
        const int rc = pthread_create(&thread_, &attr, &NgxWorker::threadEntry, this);
        pthread_attr_destroy(&attr);
        if (rc == 0) {
            started_ = true;
            logNative("ngxWorker: spawned LinRTX-NGX pthread (attr stack=%zu bytes)", requestedStack);
        } else {
            logNative("ngxWorker: pthread_create failed rc=%d; running inline", rc);
        }
    }

    void submit(Task* task) {
        std::unique_lock<std::mutex> lock(mu_);
        if (!head_) {
            head_ = tail_ = task;
        } else {
            tail_->next = task;
            tail_ = task;
        }
        cv_.notify_one();
        doneCv_.wait(lock, [&]() { return *task->done; });
    }

    static void* threadEntry(void* arg) {
        static_cast<NgxWorker*>(arg)->workerLoop();
        return nullptr;
    }

    void workerLoop() {
#ifdef __linux__
        pthread_setname_np(pthread_self(), "LinRTX-NGX");
        pthread_attr_t liveAttr{};
        if (pthread_getattr_np(pthread_self(), &liveAttr) == 0) {
            size_t liveStack = 0;
            pthread_attr_getstacksize(&liveAttr, &liveStack);
            pthread_attr_destroy(&liveAttr);
            logNative("ngxWorker: live native stack=%zu bytes", liveStack);
        }
#endif
        while (true) {
            Task* t = nullptr;
            {
                std::unique_lock<std::mutex> lock(mu_);
                cv_.wait(lock, [&]() { return head_ != nullptr || stopRequested_; });
                if (!head_ && stopRequested_) {
                    break;
                }
                t = head_;
                head_ = t->next;
                if (!head_) {
                    tail_ = nullptr;
                }
            }
            if (t && t->fn) {
                t->fn();
            }
            {
                std::lock_guard<std::mutex> lock(mu_);
                *t->done = true;
                doneCv_.notify_all();
            }
        }
    }

    std::mutex mu_;
    std::condition_variable cv_;
    std::condition_variable doneCv_;
    pthread_t thread_{};
    bool started_ = false;
    bool stopRequested_ = false;
    Task* head_ = nullptr;
    Task* tail_ = nullptr;
};

inline NgxWorker& ngxWorker() {
    static NgxWorker instance;
    return instance;
}

} // namespace linrtx
