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

#include "StdAfx.h"

#include "video_channel.h"

#include "video_format.h"

#include "consumer/output.h"
#include "frame/draw_frame.h"
#include "frame/frame.h"
#include "frame/frame_factory.h"
#include "frame/frame_transform.h"
#include "mixer/mixer.h"
#include "producer/stage.h"

#include <common/diagnostics/graph.h>
#include <common/env.h>
#include <common/executor.h>
#include <common/future.h>
#include <common/timer.h>

#include <core/diagnostics/call_context.h>
#include <core/mixer/image/image_mixer.h>

#include <boost/lexical_cast.hpp>
#include <boost/thread/lock_guard.hpp>

#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace caspar { namespace core {

bool operator<(const route_id& a, const route_id& b) { return (a.mode + (a.index << 2)) < (b.mode + (b.index << 2)); }

template <typename T, typename F>
auto lock(T& mutex, F&& func) -> decltype(func())
{
    boost::lock_guard<T> lock(mutex);
    return func();
}

struct video_channel::impl final
{
    monitor::state state_;

    const int index_;

    mutable std::mutex                format_desc_mutex_;
    core::video_format_desc           format_desc_;
    std::shared_ptr<channel_timecode> timecode_;

    const spl::shared_ptr<caspar::diagnostics::graph> graph_ = [](int index) {
        core::diagnostics::scoped_call_context save;
        core::diagnostics::call_context::for_thread().video_channel = index;
        return spl::make_shared<caspar::diagnostics::graph>();
    }(index_);

    caspar::core::output         output_;
    spl::shared_ptr<image_mixer> image_mixer_;
    caspar::core::mixer          mixer_;
    std::shared_ptr<core::stage> stage_;

    std::vector<int> audio_cadence_ = format_desc_.audio_cadence;

    std::function<void(core::monitor::state)> tick_;

    std::map<route_id, std::weak_ptr<core::route>> routes_;
    std::mutex                                     routes_mutex_;

    mutable tbb::spin_mutex timecode_listeners_mutex_;
    int64_t                 last_timecode_listener_id = 0;
    std::unordered_map<int64_t, std::function<void(core::frame_timecode, spl::shared_ptr<caspar::diagnostics::graph>)>>
        timecode_listeners_;

    std::atomic<bool> abort_request_{false};
    std::thread       thread_;

  public:
    impl(int                                       index,
         const core::video_format_desc&            format_desc,
         std::unique_ptr<image_mixer>              image_mixer,
         std::function<void(core::monitor::state)> tick)
        : index_(index)
        , format_desc_(format_desc)
        , timecode_(std::make_shared<channel_timecode>(index, format_desc))
        , output_(graph_, format_desc, index)
        , image_mixer_(std::move(image_mixer))
        , mixer_(index, graph_, image_mixer_)
        , stage_(std::make_shared<core::stage>(index, graph_))
        , tick_(tick)
    {
        graph_->set_color("produce-time", caspar::diagnostics::color(0.0f, 1.0f, 0.0f));
        graph_->set_color("mix-time", caspar::diagnostics::color(1.0f, 0.0f, 0.9f, 0.8f));
        graph_->set_color("consume-time", caspar::diagnostics::color(1.0f, 0.4f, 0.0f, 0.8f));
        graph_->set_color("frame-time", caspar::diagnostics::color(1.0f, 0.4f, 0.4f, 0.8f));
        graph_->set_color("osc-time", caspar::diagnostics::color(0.3f, 0.4f, 0.0f, 0.8f));
        graph_->set_color("skipped-schedule", caspar::diagnostics::color(0.3f, 0.6f, 0.6f));
        graph_->set_text(print());
        caspar::diagnostics::register_graph(graph_);

        // Sync the timecode with current time
        timecode_->start();

        CASPAR_LOG(info) << print() << " Successfully Initialized.";

        thread_ = std::thread([=] {
#ifdef WIN32
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
#endif
            set_thread_name(L"channel-" + boost::lexical_cast<std::wstring>(index_));

            while (!abort_request_) {
                try {
                    core::video_format_desc format_desc;
                    int                     nb_samples;
                    {
                        std::lock_guard<std::mutex> lock(format_desc_mutex_);
                        format_desc = format_desc_;
                        boost::range::rotate(audio_cadence_, std::end(audio_cadence_) - 1);
                        nb_samples = audio_cadence_.front();
                    }

                    caspar::timer frame_timer;

                    // Determine all layers that need a frame from the background producer
                    std::vector<int> background_routes = {};
                    {
                        std::lock_guard<std::mutex> lock(routes_mutex_);

                        for (auto& r : routes_) {
                            // Ensure pointer is still valid
                            if (!r.second.lock())
                                continue;

                            if (r.first.mode != route_mode::foreground) {
                                background_routes.push_back(r.first.index);
                            }
                        }
                    }

                    // Predict the new timecode for any producers to use
                    timecode_->tick(false);

                    // Produce
                    caspar::timer produce_timer;
                    auto          stage_frames = (*stage_)(format_desc, nb_samples, background_routes);
                    graph_->set_value("produce-time", produce_timer.elapsed() * format_desc.fps * 0.5);

                    // Ensure it is accurate now the producer has run
                    auto timecode = timecode_->tick(true);

                    // Schedule commands for next timecode
                    invoke_timecode_listeners(timecode);

                    // Mix
                    caspar::timer mix_timer;

                    std::vector<core::draw_frame> frames;
                    for (auto& p : stage_frames) {
                        frames.push_back(p.second.foreground);
                    }

                    auto mixed_frame = mixer_(frames, format_desc, format_desc.audio_cadence[0]);
                    graph_->set_value("mix-time", mix_timer.elapsed() * format_desc.fps * 0.5);

                    // Consume
                    caspar::timer consume_timer;
                    output_(timecode, std::move(mixed_frame), format_desc);
                    graph_->set_value("consume-time", consume_timer.elapsed() * format_desc.fps * 0.5);

                    graph_->set_value("frame-time", frame_timer.elapsed() * format_desc.fps * 0.5);

                    {
                        std::lock_guard<std::mutex> lock(routes_mutex_);

                        for (auto& r : routes_) {
                            auto route = r.second.lock();
                            if (!route) {
                                continue;
                            }

                            if (r.first.index == -1) {
                                route->signal(core::draw_frame(std::move(frames)));
                                continue;
                            }

                            auto it = stage_frames.find(r.first.index);
                            if (it == stage_frames.end()) {
                                // Layer doesnt exist, so send empty frame to avoid freezing on last
                                route->signal(draw_frame{});
                            } else {
                                if (r.first.mode == route_mode::background ||
                                    (r.first.mode == route_mode::next && it->second.has_background)) {
                                    route->signal(draw_frame::pop(it->second.background));
                                } else {
                                    route->signal(draw_frame::pop(it->second.foreground));
                                }
                            }
                        }
                    }

                    monitor::state state = {};
                    state["stage"]       = stage_->state();
                    state["mixer"]       = mixer_.state();
                    state["output"]      = output_.state();
                    state["framerate"]   = {format_desc_.framerate.numerator(), format_desc_.framerate.denominator()};
                    state["timecode"]    = timecode.string(false);
                    state["timecode/smpte"]  = timecode.string(true);
                    state["timecode/source"] = timecode_->source_name();
                    state_                   = state;

                    caspar::timer osc_timer;
                    tick_(state_);
                    graph_->set_value("osc-time", osc_timer.elapsed() * format_desc.fps * 0.5);
                } catch (...) {
                    CASPAR_LOG_CURRENT_EXCEPTION();
                }
            }
        });
    }

    ~impl()
    {
        CASPAR_LOG(info) << print() << " Uninitializing.";
        abort_request_ = true;
        thread_.join();
    }

    std::shared_ptr<core::route> route(int index = -1, route_mode mode = route_mode::foreground)
    {
        std::lock_guard<std::mutex> lock(routes_mutex_);

        route_id id = {};
        id.index    = index;
        id.mode     = mode;

        auto route = routes_[id].lock();
        if (!route) {
            route              = std::make_shared<core::route>();
            route->format_desc = format_desc_;
            route->name        = boost::lexical_cast<std::wstring>(index_);
            if (index != -1) {
                route->name += L"/" + boost::lexical_cast<std::wstring>(index);
            }
            if (mode == route_mode::background) {
                route->name += L"/background";
            } else if (mode == route_mode::next) {
                route->name += L"/next";
            }
            routes_[id] = route;
        }

        return route;
    }

    core::video_format_desc video_format_desc() const
    {
        std::lock_guard<std::mutex> lock(format_desc_mutex_);
        return format_desc_;
    }

    void video_format_desc(const core::video_format_desc& format_desc)
    {
        std::lock_guard<std::mutex> lock(format_desc_mutex_);
        format_desc_   = format_desc;
        audio_cadence_ = format_desc_.audio_cadence;
        timecode_->change_format(format_desc);
        stage_->clear();
    }

    void invoke_timecode_listeners(const core::frame_timecode& timecode)
    {
        auto listeners = lock(timecode_listeners_mutex_, [=] { return timecode_listeners_; });

        for (auto listener : listeners) {
            try {
                listener.second(timecode, graph_);
            } catch (...) {
                CASPAR_LOG_CURRENT_EXCEPTION();
            }
        }
    }

    std::shared_ptr<void> add_timecode_listener(
        std::function<void(core::frame_timecode, spl::shared_ptr<caspar::diagnostics::graph>)> listener)
    {
        return lock(timecode_listeners_mutex_, [&] {
            auto listener_id = last_timecode_listener_id++;
            timecode_listeners_.insert(std::make_pair(listener_id, listener));

            return std::shared_ptr<void>(nullptr, [=](void*) {
                lock(timecode_listeners_mutex_, [&] { timecode_listeners_.erase(listener_id); });
            });
        });
    }

    std::wstring print() const
    {
        return L"video_channel[" + boost::lexical_cast<std::wstring>(index_) + L"|" + video_format_desc().name + L"]";
    }

    int index() const { return index_; }
};

video_channel::video_channel(int                                       index,
                             const core::video_format_desc&            format_desc,
                             std::unique_ptr<image_mixer>              image_mixer,
                             std::function<void(core::monitor::state)> tick)
    : impl_(new impl(index, format_desc, std::move(image_mixer), tick))
{
}
video_channel::~video_channel() {}
const std::shared_ptr<core::stage>& video_channel::stage() const { return impl_->stage_; }
std::shared_ptr<core::stage>&       video_channel::stage() { return impl_->stage_; }
const mixer&                        video_channel::mixer() const { return impl_->mixer_; }
mixer&                              video_channel::mixer() { return impl_->mixer_; }
const output&                       video_channel::output() const { return impl_->output_; }
output&                             video_channel::output() { return impl_->output_; }
spl::shared_ptr<frame_factory>      video_channel::frame_factory() { return impl_->image_mixer_; }
core::video_format_desc             video_channel::video_format_desc() const { return impl_->video_format_desc(); }
void                                core::video_channel::video_format_desc(const core::video_format_desc& format_desc)
{
    impl_->video_format_desc(format_desc);
}
int                  video_channel::index() const { return impl_->index(); }
core::monitor::state video_channel::state() const { return impl_->state_; }

std::shared_ptr<route> video_channel::route(int index, route_mode mode) { return impl_->route(index, mode); }
std::shared_ptr<void>  video_channel::add_timecode_listener(
    std::function<void(core::frame_timecode, spl::shared_ptr<caspar::diagnostics::graph>)> listener)
{
    return impl_->add_timecode_listener(std::move(listener));
}
std::shared_ptr<core::channel_timecode> video_channel::timecode() const { return impl_->timecode_; }

}} // namespace caspar::core
