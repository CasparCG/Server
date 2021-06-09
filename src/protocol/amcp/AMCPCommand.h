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
#include "amcp_shared.h"
#include <accelerator/accelerator.h>
#include <core/consumer/frame_consumer.h>
#include <core/producer/frame_producer.h>

#include <boost/algorithm/string.hpp>

namespace caspar { namespace protocol { namespace amcp {

struct command_context
{
    IO::ClientInfoPtr                                    client;
    channel_context                                      channel;
    int                                                  channel_index;
    int                                                  layer_id;
    std::vector<channel_context>                         channels;
    spl::shared_ptr<core::cg_producer_registry>          cg_registry;
    spl::shared_ptr<const core::frame_producer_registry> producer_registry;
    spl::shared_ptr<const core::frame_consumer_registry> consumer_registry;
    std::function<void(bool)>                            shutdown_server_now;
    std::vector<std::wstring>                            parameters;
    std::string                                          proxy_host;
    std::string                                          proxy_port;
    std::weak_ptr<accelerator::accelerator_device>       ogl_device;

    int layer_index(int default_ = 0) const { return layer_id == -1 ? default_ : layer_id; }

    command_context(IO::ClientInfoPtr                                    client,
                    channel_context                                      channel,
                    int                                                  channel_index,
                    int                                                  layer_id,
                    std::vector<channel_context>                         channels,
                    spl::shared_ptr<core::cg_producer_registry>          cg_registry,
                    spl::shared_ptr<const core::frame_producer_registry> producer_registry,
                    spl::shared_ptr<const core::frame_consumer_registry> consumer_registry,
                    std::function<void(bool)>                            shutdown_server_now,
                    std::string                                          proxy_host,
                    std::string                                          proxy_port,
                    std::weak_ptr<accelerator::accelerator_device>       ogl_device)
        : client(std::move(client))
        , channel(channel)
        , channel_index(channel_index)
        , layer_id(layer_id)
        , channels(std::move(channels))
        , cg_registry(std::move(cg_registry))
        , producer_registry(std::move(producer_registry))
        , consumer_registry(std::move(consumer_registry))
        , shutdown_server_now(shutdown_server_now)
        , proxy_host(std::move(proxy_host))
        , proxy_port(std::move(proxy_port))
        , ogl_device(std::move(ogl_device))
    {
    }
};

using amcp_command_func = std::function<std::wstring(command_context& args)>;

class AMCPCommand
{
  private:
    command_context         ctx_;
    const amcp_command_func command_;
    const int               min_num_params_;
    const std::wstring      name_;
    const std::wstring      request_id_;

  public:
    AMCPCommand(const command_context&     ctx,
                const amcp_command_func& command,
                int                      min_num_params,
                const std::wstring&      name,
                const std::wstring& request_id)
        : ctx_(ctx)
        , command_(command)
        , min_num_params_(min_num_params)
        , name_(name)
        , request_id_(request_id)
    {
    }

    using ptr_type = std::shared_ptr<AMCPCommand>;

    const std::wstring Execute()
    {
        return command_(ctx_);
    }

    int minimum_parameters() const { return min_num_params_; }

    void SendReply(std::wstring replyString)
    {
        if (replyString.empty())
            return;

        if (!request_id_.empty())
            replyString = L"RES " + request_id_ + L" " + replyString;

        ctx_.client->send(std::move(replyString));
    }

    const std::vector<std::wstring>& parameters() const { return ctx_.parameters; }

    int channel_index() const { return ctx_.channel_index; }

    IO::ClientInfoPtr client() const { return ctx_.client; }

    const std::wstring& print() const { return name_; }
};
}}} // namespace caspar::protocol::amcp
