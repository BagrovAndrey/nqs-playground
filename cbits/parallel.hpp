#include "config.hpp"
#include "errors.hpp"
#include <taskflow/taskflow.hpp>

#include <ATen/Parallel.h>

#include <omp.h>

#if 0
#    include <condition_variable>
#    include <functional>
#    include <future>
#    include <memory>
#    include <mutex>
#    include <queue>
#    include <stdexcept>
#    include <thread>
#    include <vector>
#endif

TCM_NAMESPACE_BEGIN

auto global_executor() noexcept -> tf::Executor&;

#if 0
// Copyright (c) 2012 Jakob Progsch, Václav Zeman
//
// This software is provided 'as-is', without any express or implied
// warranty. In no event will the authors be held liable for any damages
// arising from the use of this software.
//
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it
// freely, subject to the following restrictions:
//
//    1. The origin of this software must not be misrepresented; you must not
//    claim that you wrote the original software. If you use this software
//    in a product, an acknowledgment in the product documentation would be
//    appreciated but is not required.
//
//    2. Altered source versions must be plainly marked as such, and must not be
//    misrepresented as being the original software.
//
//    3. This notice may not be removed or altered from any source
//    distribution.
//
// The following is a small adaptation of https://github.com/progschj/ThreadPool
// got a single worker thread.
class ThreadPool {
  public:
    ThreadPool();

    template <class F>
    auto enqueue(F&& f) -> std::future<typename std::result_of<F()>::type>
    {
        using return_type = typename std::result_of<F()>::type;
        auto task         = std::make_shared<std::packaged_task<return_type()>>(
            std::forward<F>(f));
        std::future<return_type> res = task->get_future();
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            // don't allow enqueueing after stopping the pool
            TCM_CHECK(!stop, std::runtime_error,
                      "enqueue on stopped ThreadPool");
            tasks.emplace([p = std::move(task)]() { (*p)(); });
        }
        condition.notify_one();
        return res;
    }

    ~ThreadPool();

  private:
    // need to keep track of threads so we can join them
    std::thread worker;
    // task queue
    std::queue<std::function<void()>> tasks;
    // synchronization
    std::mutex              queue_mutex;
    std::condition_variable condition;
    bool                    stop;
};

namespace detail {
auto global_thread_pool() noexcept -> ThreadPool&;
} // namespace detail
#endif

template <class F> auto async(F&& f)
{
    using R     = typename std::result_of<F()>::type;
    auto task   = std::make_shared<std::packaged_task<R()>>(std::forward<F>(f));
    auto future = task->get_future();
    at::launch([p = std::move(task)]() { (*p)(); });
    return future;
    // return detail::global_thread_pool().enqueue(std::forward<F>(f));
}

struct omp_task_handler {
  private:
    std::atomic_flag   _error_flag    = ATOMIC_FLAG_INIT;
    std::exception_ptr _exception_ptr = nullptr;

  public:
    template <class F> auto submit(F f) -> void
    {
        static_assert(std::is_nothrow_copy_constructible<F>::value,
                      TCM_STATIC_ASSERT_BUG_MESSAGE);
#pragma omp task default(none) firstprivate(f)                                 \
    shared(_error_flag, _exception_ptr)
        {
            try {
                f();
            }
            catch (...) {
                if (!_error_flag.test_and_set()) {
                    _exception_ptr = std::current_exception();
                }
            }
        }
    }

    auto check_errors() const -> void
    {
        if (_exception_ptr) { std::rethrow_exception(_exception_ptr); }
    }
};

TCM_NAMESPACE_END
