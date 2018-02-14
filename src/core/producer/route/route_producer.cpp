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

#include <common/scope_exit.h>

#include <core/frame/draw_frame.h>
#include <core/monitor/monitor.h>
#include <core/producer/frame_producer.h>
#include <core/video_channel.h>

#include <boost/regex.hpp>
#include <boost/signals2.hpp>
#include <boost/range/algorithm/find_if.hpp>

namespace caspar { namespace core {

class route_producer : public frame_producer_base
{
    monitor::state state_;

    std::shared_ptr<route> route_;
    boost::signals2::connection connection_;

    core::draw_frame frame_;
    std::mutex       frame_mutex_;

  public:
    route_producer(std::shared_ptr<route> route)
        : route_(route)
        , connection_(route_->signal.connect([this](const core::draw_frame& frame) {
            std::lock_guard<std::mutex> lock(frame_mutex_);
            frame_ = frame;
        }))
    {
        CASPAR_LOG(debug) << print() << L" Initialized";
    }

    draw_frame receive_impl() override
    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        return std::move(frame_);
    }

    std::wstring print() const override
    {
        return L"route[" + route_->name + L"]";
    }

    std::wstring name() const override { return L"route"; }

    const monitor::state& state() const { return state_; }
};

spl::shared_ptr<core::frame_producer> create_route_producer(const core::frame_producer_dependencies& dependencies,
                                                            const std::vector<std::wstring>&         params)
{
    static boost::wregex expr(L"route://(?<CHANNEL>\\d+)(-(?<LAYER>\\d+))?");
    boost::wsmatch what;

    if (params.empty() || !boost::regex_match(params.at(0), what, expr)) {
        return core::frame_producer::empty();
    }

    auto channel = boost::lexical_cast<int>(what["CHANNEL"].str());
    auto layer   = what["LAYER"].matched ? boost::lexical_cast<int>(what["LAYER"].str()) : -1;

    auto channel_it = boost::find_if(dependencies.channels, [=](spl::shared_ptr<core::video_channel> ch) { return ch->index() == channel; });

    if (channel_it == dependencies.channels.end()) {
        CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"No channel with id " + boost::lexical_cast<std::wstring>(channel)));
    }

    return spl::make_shared<route_producer>((*channel_it)->route(layer));
}

}} // namespace caspar::core
