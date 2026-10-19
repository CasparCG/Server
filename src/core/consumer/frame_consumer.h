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

#include <common/bit_depth.h>
#include <common/memory.h>

#include <common/future.h>
#include <core/video_format.h>

#include <boost/property_tree/ptree_fwd.hpp>

#include <functional>
#include <future>
#include <string>
#include <vector>

namespace caspar { namespace core {

class frame_consumer
{
    frame_consumer(const frame_consumer&);
    frame_consumer& operator=(const frame_consumer&);

    std::wstring consumer_id_;

  public:
    static const spl::shared_ptr<frame_consumer>& empty();

    frame_consumer() {}
    virtual ~frame_consumer() {}

    virtual std::future<bool> send(const core::video_field field, const_frame frame) = 0;
    virtual void
    initialize(const video_format_desc& format_desc, const core::channel_info& channel_info, int port_index) = 0;
    virtual std::future<bool> call(const std::vector<std::wstring>& params) { return caspar::make_ready_future(false); }

    virtual core::monitor::state state() const = 0;

    virtual std::wstring print() const = 0;
    virtual std::wstring name() const  = 0;
    virtual bool         has_synchronization_clock() const { return false; }
    virtual int          index() const = 0;

    const std::wstring&  consumer_id() const { return consumer_id_; }
    virtual void         set_consumer_id(const std::wstring& id) { consumer_id_ = id; }
};

}} // namespace caspar::core
