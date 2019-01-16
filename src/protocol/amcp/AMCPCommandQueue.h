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
 * Author: Nicklas P Andersson
 */

#pragma once

#include "AMCPCommand.h"

#include <common/executor.h>
#include <common/memory.h>

namespace caspar { namespace protocol { namespace amcp {

class AMCPCommandQueue
{
  public:
    typedef spl::shared_ptr<AMCPCommandQueue> ptr_type;

    AMCPCommandQueue(const std::wstring& name, const std::vector<channel_context>& channels);
    ~AMCPCommandQueue();

    void AddCommand(std::shared_ptr<AMCPGroupCommand> command);
    void Execute(std::shared_ptr<AMCPGroupCommand> cmd) const;

  private:
    executor                            executor_;
    const std::vector<channel_context>& channels_;
};

}}} // namespace caspar::protocol::amcp
