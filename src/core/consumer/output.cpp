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
#include "output.h"

#include "frame_consumer.h"

#include "../frame/frame.h"
#include "../monitor/monitor.h"
#include "../video_format.h"

#include <common/diagnostics/graph.h>
#include <common/except.h>
#include <common/memory.h>

#include <boost/optional.hpp>

#include <chrono>
#include <map>
#include <thread>

namespace caspar { namespace core {

typedef decltype(std::chrono::high_resolution_clock::now()) time_point_t;

struct output::impl
{
    monitor::state                      state_;
    spl::shared_ptr<diagnostics::graph> graph_;
    const int                           channel_index_;
    video_format_desc                   format_desc_;

    std::mutex                                     consumers_mutex_;
    std::map<int, spl::shared_ptr<frame_consumer>> consumers_;

    boost::optional<time_point_t> time_;

  public:
    impl(spl::shared_ptr<diagnostics::graph> graph, const video_format_desc& format_desc, int channel_index)
        : graph_(std::move(graph))
        , channel_index_(channel_index)
        , format_desc_(format_desc)
    {
    }

    void add(int index, spl::shared_ptr<frame_consumer> consumer)
    {
        remove(index);

        consumer->initialize(format_desc_, channel_index_);

        std::lock_guard<std::mutex> lock(consumers_mutex_);
        consumers_.emplace(index, std::move(consumer));
    }

    void add(const spl::shared_ptr<frame_consumer>& consumer) { add(consumer->index(), consumer); }

    bool remove(int index)
    {
        std::lock_guard<std::mutex> lock(consumers_mutex_);
        auto                        it = consumers_.find(index);
        if (it != consumers_.end()) {
            consumers_.erase(it);
            return true;
        }
        return false;
    }

    bool remove(const spl::shared_ptr<frame_consumer>& consumer) { return remove(consumer->index()); }

    void operator()(frame_timecode timecode, const_frame input_frame, const core::video_format_desc& format_desc)
    {
        if (!input_frame) {
            return;
        }

        if (input_frame.size() != format_desc_.size) {
            CASPAR_LOG(warning) << print() << L" Invalid input frame size.";
            return;
        }

        auto time = std::move(time_);

        if (format_desc_ != format_desc) {
            std::lock_guard<std::mutex> lock(consumers_mutex_);
            for (auto it = consumers_.begin(); it != consumers_.end();) {
                try {
                    it->second->initialize(format_desc, it->first);
                    ++it;
                } catch (...) {
                    CASPAR_LOG_CURRENT_EXCEPTION();
                    it = consumers_.erase(it);
                }
            }
            format_desc_ = format_desc;
            time_        = boost::none;
            return;
        }

        decltype(consumers_) consumers;
        {
            std::lock_guard<std::mutex> lock(consumers_mutex_);
            consumers = consumers_;
        }

        std::map<int, std::future<bool>> futures;

        for (auto it = consumers_.begin(); it != consumers_.end();) {
            try {
                futures.emplace(it->first, it->second->send(timecode, input_frame));
                ++it;
            } catch (...) {
                CASPAR_LOG_CURRENT_EXCEPTION();
                it = consumers_.erase(it);
            }
        }

        for (auto& p : futures) {
            try {
                if (!p.second.get()) {
                    consumers_.erase(p.first);
                }
            } catch (...) {
                CASPAR_LOG_CURRENT_EXCEPTION();
                consumers_.erase(p.first);
            }
        }

        monitor::state state;
        for (auto& p : consumers_) {
            state["port"][p.first] = p.second->state();
        }
        state_ = std::move(state);

        const auto needs_sync = std::all_of(
            consumers_.begin(), consumers_.end(), [](auto& p) { return !p.second->has_synchronization_clock(); });

        if (needs_sync) {
            if (!time) {
                time = std::chrono::high_resolution_clock::now();
            } else {
                std::this_thread::sleep_until(*time);
            }
            time_ = *time + std::chrono::microseconds(static_cast<int>(1e6 / format_desc_.fps));
        } else {
            time_.reset();
        }
    }

    std::wstring print() const { return L"output[" + boost::lexical_cast<std::wstring>(channel_index_) + L"]"; }
};

output::output(spl::shared_ptr<diagnostics::graph> graph, const video_format_desc& format_desc, int channel_index)
    : impl_(new impl(std::move(graph), format_desc, channel_index))
{
}
output::~output() {}
void output::add(int index, const spl::shared_ptr<frame_consumer>& consumer) { impl_->add(index, consumer); }
void output::add(const spl::shared_ptr<frame_consumer>& consumer) { impl_->add(consumer); }
bool output::remove(int index) { return impl_->remove(index); }
bool output::remove(const spl::shared_ptr<frame_consumer>& consumer) { return impl_->remove(consumer); }
void output::operator()(frame_timecode timecode, const_frame frame, const video_format_desc& format_desc)
{
    return (*impl_)(timecode, std::move(frame), format_desc);
}
core::monitor::state output::state() const { return impl_->state_; }
}} // namespace caspar::core
