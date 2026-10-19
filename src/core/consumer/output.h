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

#include "../fwd.h"
#include "../monitor/monitor.h"

#include <common/memory.h>
#include <core/video_format.h>

#include <memory>
#include <string>

namespace caspar::diagnostics {
class graph;
}

namespace caspar { namespace core {

class output final
{
  public:
    explicit output(const spl::shared_ptr<diagnostics::graph>& graph,
                    const video_format_desc&                   format_desc,
                    const core::channel_info&                  channel_info);

    output(const output&)            = delete;
    output& operator=(const output&) = delete;
    ~output();

    // Send a frame to the output. If running an interlaced channel, two frames will be provided
    void operator()(const const_frame& frame, const const_frame& frame2, const video_format_desc& format_desc);

    void add(const spl::shared_ptr<frame_consumer>& consumer);
    void add(int index, const spl::shared_ptr<frame_consumer>& consumer);
    bool remove(const spl::shared_ptr<frame_consumer>& consumer);
    bool remove(int index);
    bool remove_by_id(const std::wstring& consumer_id);

    std::future<bool> call(int index, const std::vector<std::wstring>& params);

    size_t consumer_count() const;

    core::monitor::state state() const;

  private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

}} // namespace caspar::core
