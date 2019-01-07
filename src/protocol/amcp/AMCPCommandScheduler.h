/*
 * Copyright (c) 2018 Norsk rikskringkasting AS
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
 * Author: Julian Waller, julian@superfly.tv
 */

#pragma once

#include "AMCPCommand.h"

#include <core/frame/frame_timecode.h>

namespace caspar { namespace protocol { namespace amcp {

class AMCPCommandScheduler
{
  public:
    AMCPCommandScheduler();

    void create_channels(int count);

    void set(int                          channel_index,
             const std::wstring&          token,
             const core::frame_timecode&  timecode,
             std::shared_ptr<AMCPCommand> command);

    bool remove(const std::wstring& token);

    void clear();

    std::vector<std::tuple<int, core::frame_timecode, std::wstring>> list(int                   channel_index,
                                                                          core::frame_timecode& timecode);
    std::vector<std::tuple<int, core::frame_timecode, std::wstring>> list_all();

    std::pair<core::frame_timecode, std::shared_ptr<AMCPCommand>> find(const std::wstring& token);

    boost::optional<std::vector<std::shared_ptr<AMCPGroupCommand>>> schedule(int                  channel_index,
                                                                             core::frame_timecode timecode);

  private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

}}} // namespace caspar::protocol::amcp