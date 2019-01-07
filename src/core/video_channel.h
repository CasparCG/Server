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

#include "fwd.h"
#include "video_format.h"

#include "channel_timecode.h"
#include "frame/frame_timecode.h"
#include "monitor/monitor.h"

#include <common/forward.h>
#include <common/memory.h>

#include <boost/signals2.hpp>

#include <common/diagnostics/graph.h>
#include <functional>

namespace caspar { namespace core {

enum route_mode
{
    foreground,
    background,
    next, // background if any, otherwise foreground
};

struct route_id
{
    int        index;
    route_mode mode;

    bool const operator==(const route_id& o) { return index == o.index && mode == o.mode; }
};

struct route
{
    route()             = default;
    route(const route&) = delete;
    route(route&&)      = default;

    route& operator=(const route&) = delete;
    route& operator=(route&&) = default;

    boost::signals2::signal<void(class draw_frame)> signal;
    video_format_desc                               format_desc;
    std::wstring                                    name;
};

class video_channel final
{
    video_channel(const video_channel&);
    video_channel& operator=(const video_channel&);

  public:
    explicit video_channel(int                                       index,
                           const video_format_desc&                  format_desc,
                           std::unique_ptr<image_mixer>              image_mixer,
                           std::function<void(core::monitor::state)> on_tick);
    ~video_channel();

    core::monitor::state                    state() const;
    std::shared_ptr<core::channel_timecode> timecode() const;

    const std::shared_ptr<core::stage>& stage() const;
    std::shared_ptr<core::stage>&       stage();
    const core::mixer&                  mixer() const;
    core::mixer&                        mixer();
    const core::output&                 output() const;
    core::output&                       output();

    core::video_format_desc video_format_desc() const;
    void                    video_format_desc(const core::video_format_desc& format_desc);

    spl::shared_ptr<core::frame_factory> frame_factory();

    std::shared_ptr<void> add_timecode_listener(
        std::function<void(core::frame_timecode, spl::shared_ptr<caspar::diagnostics::graph>)> listener);

    int index() const;

    std::shared_ptr<core::route> route(int index = -1, route_mode mode = route_mode::foreground);

  private:
    struct impl;
    spl::unique_ptr<impl> impl_;
};

}} // namespace caspar::core
