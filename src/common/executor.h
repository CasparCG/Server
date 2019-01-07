/*
 * Copyright (c) 2011 Sveriges Television AB <info@casparcg.com>
 *
 * This file is part of CasparCG (www.casparcg.com).
 *
 * CasparCG is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * CasparCG is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with CasparCG. If not, see <http://www.gnu.org/licenses/>.
 *
 * Author: Robert Nagy, ronag89@gmail.com
 */

#pragma once

#include "except.h"
#include "log.h"
#include "os/thread.h"

#include <tbb/concurrent_queue.h>

#include <atomic>
#include <functional>
#include <future>

namespace caspar {

class executor final
{
    executor(const executor&);
    executor& operator=(const executor&);

    typedef std::function<void()>                 task_t;
    typedef tbb::concurrent_bounded_queue<task_t> queue_t;

    std::wstring      name_;
    std::atomic<bool> is_running_{true};
    queue_t           queue_;
    std::thread       thread_;

  public:
    executor(const std::wstring& name)
        : name_(name)
        , thread_(std::thread([this] { run(); }))
    {
    }

    ~executor() { stop_and_wait(); }

    template <typename Func>
    auto begin_invoke(Func&& func)
    {
        if (!is_running_) {
            CASPAR_THROW_EXCEPTION(invalid_operation() << msg_info("executor not running."));
        }

        typedef decltype(func()) result_type;

        auto task = std::make_shared<std::packaged_task<result_type()>>(std::forward<Func>(func));

        queue_.push([=]() mutable { (*task)(); });

        return task->get_future();
    }

    template <typename Func>
    auto invoke(Func&& func)
    {
        if (is_current()) { // Avoids potential deadlock.
            return func();
        }

        return begin_invoke(std::forward<Func>(func)).get();
    }

    template <typename Func>
    typename std::enable_if<std::is_same<void, decltype(std::declval<Func>())>::value, void>::type invoke(Func&& func)
    {
        if (is_current()) { // Avoids potential deadlock.
            func();
        }

        begin_invoke(std::forward<Func>(func)).wait();
    }

    void yield() {}

    void set_capacity(queue_t::size_type capacity) { queue_.set_capacity(capacity); }

    queue_t::size_type capacity() const { return queue_.capacity(); }

    void clear() { queue_.clear(); }

    void stop()
    {
        if (!is_running_) {
            return;
        }
        is_running_ = false;
        queue_.push(nullptr);
    }

    void stop_and_wait()
    {
        stop();

        if (thread_.joinable())
            thread_.join();
    }

    void wait()
    {
        invoke([] {});
    }

    queue_t::size_type size() const { return queue_.size(); }

    bool is_running() const { return is_running_; }

    bool is_current() const { return std::this_thread::get_id() == thread_.get_id(); }

    const std::wstring& name() const { return name_; }

  private:
    void run()
    {
        set_thread_name(name_);

        task_t task;

        while (is_running_) {
            try {
                queue_.pop(task);
                do {
                    if (!task) {
                        return;
                    }
                    task();
                } while (queue_.try_pop(task));
            } catch (...) {
                CASPAR_LOG_CURRENT_EXCEPTION();
            }
        }
    }
};

} // namespace caspar