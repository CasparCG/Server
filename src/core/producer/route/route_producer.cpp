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
#include "route_producer.h"

#include <common/diagnostics/graph.h>
#include <common/param.h>
#include <common/timer.h>
#include <stack>

#include <core/frame/frame.h>
#include <core/frame/draw_frame.h>
#include <core/frame/frame_visitor.h>
#include <core/frame/frame_transform.h>
#include <core/monitor/monitor.h>
#include <core/producer/frame_producer.h>
#include <core/video_channel.h>

#include <boost/range/algorithm/find_if.hpp>
#include <boost/regex.hpp>
#include <boost/signals2.hpp>

#include <tbb/concurrent_queue.h>

namespace caspar { namespace core {

class fix_stream_tag : public frame_visitor
{
    const void* stream_tag_;
    std::stack<std::pair<frame_transform, std::vector<draw_frame>>> frames_stack_;
    boost::optional<const_frame>                                    upd_frame_;
    
    fix_stream_tag(const fix_stream_tag&);
    fix_stream_tag& operator=(const fix_stream_tag&);

  public:
    fix_stream_tag(void* stream_tag)
        : stream_tag_(stream_tag)
    {
        frames_stack_ = std::stack<std::pair<frame_transform, std::vector<draw_frame>>>();
        frames_stack_.emplace(frame_transform{}, std::vector<draw_frame>());
    }

    void push(const frame_transform& transform) {
        frames_stack_.emplace(transform, std::vector<core::draw_frame>());
    }

    void visit(const const_frame& frame) {
        upd_frame_ = frame.with_tag(stream_tag_);
    }

    void pop() {
        auto popped = frames_stack_.top();
        frames_stack_.pop();

        if (upd_frame_ != boost::none) {
            auto new_frame        = draw_frame(std::move(*upd_frame_));
            upd_frame_            = boost::none;
            new_frame.transform() = popped.first;
            frames_stack_.top().second.push_back(std::move(new_frame));
        } else {
            auto new_frame        = draw_frame(std::move(popped.second));
            new_frame.transform() = popped.first;
            frames_stack_.top().second.push_back(new_frame);
        }
    }

    draw_frame operator()(draw_frame frame) {
        frame.accept(*this);

        auto popped = frames_stack_.top();
        frames_stack_.pop();
        draw_frame result = std::move(popped.second);

        frames_stack_ = std::stack<std::pair<frame_transform, std::vector<draw_frame>>>();
        frames_stack_.emplace(frame_transform{}, std::vector<draw_frame>());
        return result;
    }
};

class route_producer
    : public frame_producer
    , public route_control
{
    spl::shared_ptr<diagnostics::graph> graph_;

    tbb::concurrent_bounded_queue<core::draw_frame> buffer_;

    caspar::timer produce_timer_;
    caspar::timer consume_timer_;

    std::shared_ptr<route>             route_;
    boost::signals2::scoped_connection connection_;

    core::draw_frame frame_;
    int              source_channel_;
    int              source_layer_;
    fix_stream_tag   tag_fix_;

    int get_source_channel() const { return source_channel_; }
    int get_source_layer() const { return source_layer_; }

    // set the buffer depth to 2 for cross-channel routes, 1 otherwise
    void set_cross_channel(bool cross)
    {
        if (cross) {
            buffer_.set_capacity(2);
        } else {
            buffer_.set_capacity(1);
        }
    }

  public:
    route_producer(std::shared_ptr<route> route, int buffer, int source_channel, int source_layer)
        : route_(route)
        , source_channel_(source_channel)
        , source_layer_(source_layer)
        , tag_fix_(this)
        , connection_(route_->signal.connect([this](const core::draw_frame& frame) {
            auto frame2 = frame;
            if (!frame2) {
                // We got a frame, so ensure it is a real frame (otherwise the layer gets confused)
                frame2 = core::draw_frame::push(frame2);
            }
            // Update the tag in the frame to allow the audio mixer to distiguish between the source frame and the routed frame
            frame2 = tag_fix_(frame2);
            if (!buffer_.try_push(frame2)) {
                graph_->set_tag(diagnostics::tag_severity::WARNING, "dropped-frame");
            }
            graph_->set_value("produce-time", produce_timer_.elapsed() * route_->format_desc.fps * 0.5);
            produce_timer_.restart();
        }))
    {
        buffer_.set_capacity(buffer > 0 ? buffer : 1);

        graph_->set_color("late-frame", diagnostics::color(0.6f, 0.3f, 0.3f));
        graph_->set_color("produce-time", caspar::diagnostics::color(0.0f, 1.0f, 0.0f));
        graph_->set_color("consume-time", caspar::diagnostics::color(1.0f, 0.4f, 0.0f, 0.8f));
        graph_->set_color("dropped-frame", diagnostics::color(0.3f, 0.6f, 0.3f));
        graph_->set_text(print());
        diagnostics::register_graph(graph_);

        CASPAR_LOG(debug) << print() << L" Initialized";
    }

    draw_frame last_frame() override
    {
        if (!frame_) {
            buffer_.try_pop(frame_);
        }
        return core::draw_frame::still(frame_);
    }

    draw_frame receive_impl(int nb_samples) override
    {
        core::draw_frame frame;
        if (!buffer_.try_pop(frame)) {
            graph_->set_tag(diagnostics::tag_severity::WARNING, "late-frame");
        } else {
            frame_ = frame;
        }

        graph_->set_value("consume-time", consume_timer_.elapsed() * route_->format_desc.fps * 0.5);
        consume_timer_.restart();
        return frame;
    }

    std::wstring print() const override { return L"route[" + route_->name + L"]"; }

    std::wstring name() const override { return L"route"; }
};

spl::shared_ptr<core::frame_producer> create_route_producer(const core::frame_producer_dependencies& dependencies,
                                                            const std::vector<std::wstring>&         params)
{
    static boost::wregex expr(L"route://(?<CHANNEL>\\d+)(-(?<LAYER>\\d+))?", boost::regex::icase);
    boost::wsmatch       what;

    if (params.empty() || !boost::regex_match(params.at(0), what, expr)) {
        return core::frame_producer::empty();
    }

    auto channel = std::stoi(what["CHANNEL"].str());
    auto layer   = what["LAYER"].matched ? std::stoi(what["LAYER"].str()) : -1;

    auto mode = core::route_mode::foreground;
    if (layer >= 0) {
        if (contains_param(L"BACKGROUND", params))
            mode = core::route_mode::background;
        else if (contains_param(L"NEXT", params))
            mode = core::route_mode::next;
    }

    auto channel_it = boost::find_if(dependencies.channels,
                                     [=](spl::shared_ptr<core::video_channel> ch) { return ch->index() == channel; });

    if (channel_it == dependencies.channels.end()) {
        CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"No channel with id " + std::to_wstring(channel)));
    }

    auto buffer = get_param(L"BUFFER", params, 0);

    return spl::make_shared<route_producer>((*channel_it)->route(layer, mode), buffer, channel, layer);
}

}} // namespace caspar::core
