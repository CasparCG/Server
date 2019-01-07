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

#include <protocol/amcp/AMCPCommandScheduler.h>
#include <protocol/amcp/amcp_command_repository.h>

#include <boost/noncopyable.hpp>

#include <functional>
#include <memory>

namespace caspar {

class server final : public boost::noncopyable
{
  public:
    explicit server(std::function<void(bool)> shutdown_server_now);
    void                                                     start();
    spl::shared_ptr<protocol::amcp::amcp_command_repository> get_amcp_command_repository() const;
    spl::shared_ptr<protocol::amcp::AMCPCommandScheduler>    get_amcp_command_scheduler() const;

  private:
    struct impl;
    std::shared_ptr<impl> impl_;
};

} // namespace caspar