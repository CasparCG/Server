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

#include "../util/ClientInfo.h"
#include "AMCPCommandScheduler.h"
#include "amcp_command_repository.h"
#include "amcp_shared.h"
#include <core/consumer/frame_consumer.h>
#include <future>

namespace caspar { namespace protocol { namespace amcp {

struct amcp_command_static_context
{
    const spl::shared_ptr<core::cg_producer_registry>          cg_registry;
    const spl::shared_ptr<const core::frame_producer_registry> producer_registry;
    const spl::shared_ptr<const core::frame_consumer_registry> consumer_registry;
    const spl::shared_ptr<AMCPCommandScheduler>                scheduler;
    const std::shared_ptr<amcp_command_repository>             parser;
    std::function<void(bool)>                                  shutdown_server_now;
    std::string                                                proxy_host;
    std::string                                                proxy_port;

    amcp_command_static_context(const spl::shared_ptr<core::cg_producer_registry>          cg_registry,
                                const spl::shared_ptr<const core::frame_producer_registry> producer_registry,
                                const spl::shared_ptr<const core::frame_consumer_registry> consumer_registry,
                                const spl::shared_ptr<AMCPCommandScheduler>                scheduler,
                                const std::shared_ptr<amcp_command_repository>             parser,
                                std::function<void(bool)>                                  shutdown_server_now,
                                std::string                                                proxy_host,
                                std::string                                                proxy_port)
        : cg_registry(std::move(cg_registry))
        , producer_registry(std::move(producer_registry))
        , consumer_registry(std::move(consumer_registry))
        , scheduler(std::move(scheduler))
        , parser(std::move(parser))
        , shutdown_server_now(shutdown_server_now)
        , proxy_host(proxy_host)
        , proxy_port(proxy_port)
    {
    }
};

struct command_context
{
    std::shared_ptr<amcp_command_static_context> static_context;
    const std::vector<channel_context>           channels;
    const IO::ClientInfoPtr                      client;
    const channel_context                        channel;
    const int                                    channel_index;
    const int                                    layer_id;
    std::vector<std::wstring>                    parameters;

    int layer_index(int default_ = 0) const { return layer_id == -1 ? default_ : layer_id; }

    command_context(std::shared_ptr<amcp_command_static_context> static_context,
                    const std::vector<channel_context>           channels,
                    IO::ClientInfoPtr                            client,
                    channel_context                              channel,
                    int                                          channel_index,
                    int                                          layer_id)
        : static_context(static_context)
        , channels(std::move(channels))
        , client(std::move(client))
        , channel(channel)
        , channel_index(channel_index)
        , layer_id(layer_id)
    {
    }
};

}}} // namespace caspar::protocol::amcp