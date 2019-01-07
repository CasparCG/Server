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

#include "../util/ClientInfo.h"

#include <common/memory.h>

#include "AMCPCommandScheduler.h"
#include "amcp_command_repository.h"

#include <string>

namespace caspar { namespace protocol { namespace amcp {

IO::protocol_strategy_factory<char>::ptr
create_char_amcp_strategy_factory(const std::wstring&                             name,
                                  const spl::shared_ptr<amcp_command_repository>& repo,
                                  const spl::shared_ptr<AMCPCommandScheduler>&    scheduler);

IO::protocol_strategy_factory<wchar_t>::ptr
create_wchar_amcp_strategy_factory(const std::wstring&                             name,
                                   const spl::shared_ptr<amcp_command_repository>& repo,
                                   const spl::shared_ptr<AMCPCommandScheduler>&    scheduler);

}}} // namespace caspar::protocol::amcp
