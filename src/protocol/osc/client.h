/*
 * Copyright 2013 Sveriges Television AB http://casparcg.com/
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
 * Author: Helge Norberg, helge.norberg@svt.se
 */

#pragma once

#include <boost/asio/ip/udp.hpp>

#include <common/memory.h>
#include <core/monitor/monitor.h>

namespace caspar { namespace protocol { namespace osc {

class client
{
    client(const client&);
    client& operator=(const client&);

  public:
    client(std::shared_ptr<boost::asio::io_context> context);

    client(client&&);

    /**
     * Get a subscription token that ensures that OSC messages are sent to the
     * given endpoint as long as the token is alive. It will stop sending when
     * the token is dropped unless another token to the same endpoint has
     * previously been checked out.
     *
     * @param endpoint The UDP endpoint to send OSC messages to.
     *
     * @return The token. It is ok for the token to outlive the client
     */
    std::shared_ptr<void> get_subscription_token(const boost::asio::ip::udp::endpoint& endpoint);

    ~client();

    client& operator=(client&&);

    /// Queues a complete applied channel snapshot, suppressing only unchanged transform properties.
    void send(int channel_index, const core::monitor::state& state);

  private:
    struct impl;
    spl::shared_ptr<impl> impl_;
};

}}} // namespace caspar::protocol::osc
