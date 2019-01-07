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

#include <core/frame/frame_timecode.h>

namespace caspar { namespace core {

// Interface
class timecode_source
{
  public:
    virtual ~timecode_source() = default;

    virtual const frame_timecode& timecode()          = 0;
    virtual bool                  has_timecode()      = 0;
    virtual bool                  provides_timecode() = 0;

    virtual std::wstring print() const = 0;
};

}} // namespace caspar::core