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
 * Author: Helge Norberg, helge.norberg@svt.se
 */

#include "../StdAfx.h"

#include "amcp_command_repository.h"

#include <common/env.h>

#include <boost/property_tree/ptree.hpp>

#include <map>

namespace caspar { namespace protocol { namespace amcp {

AMCPCommand::ptr_type find_command(const std::map<std::wstring, std::pair<amcp_command_func, int>>& commands,
                                   const std::wstring&                                              name,
                                   const std::wstring&                                              request_id,
                                   command_context&                                                 ctx,
                                   std::list<std::wstring>&                                         tokens)
{
    std::wstring subcommand;

    if (!tokens.empty())
        subcommand = boost::to_upper_copy(tokens.front());

    // Start with subcommand syntax like MIXER CLEAR etc
    if (!subcommand.empty()) {
        auto fullname      = name + L" " + subcommand;
        auto subcmd = commands.find(fullname);

        if (subcmd != commands.end()) {
            tokens.pop_front();
            ctx.parameters = std::move(std::vector<std::wstring>(tokens.begin(), tokens.end()));
            return std::make_shared<AMCPCommand>(
                ctx, subcmd->second.first, subcmd->second.second, fullname, request_id);
        }
    }

    // Resort to ordinary command
    auto command = commands.find(name);
    if (command != commands.end()) {
        ctx.parameters = std::move(std::vector<std::wstring>(tokens.begin(), tokens.end()));
        return std::make_shared<AMCPCommand>(ctx, command->second.first, command->second.second, name, request_id);
    }

    return nullptr;
}

template <typename Out, typename In>
bool try_lexical_cast(const In& input, Out& result)
{
    Out        saved   = result;
    const bool success = boost::conversion::detail::try_lexical_convert(input, result);

    if (!success)
        result = saved; // Needed because of how try_lexical_convert is implemented.

    return success;
}

static void
parse_channel_id(std::list<std::wstring>& tokens, std::wstring& channel_spec, int& channel_index, int& layer_index)
{
    if (!tokens.empty()) {
        channel_spec                            = tokens.front();
        std::wstring              channelid_str = boost::trim_copy(channel_spec);
        std::vector<std::wstring> split;
        boost::split(split, channelid_str, boost::is_any_of("-"));

        // Use non_throwing lexical cast to not hit exception break point all the time.
        if (try_lexical_cast(split[0], channel_index)) {
            --channel_index;

            if (split.size() > 1)
                try_lexical_cast(split[1], layer_index);

            // Consume channel-spec
            tokens.pop_front();
        }
    }
}


struct amcp_command_repository::impl
{
    std::vector<channel_context>                         channels;
    spl::shared_ptr<core::cg_producer_registry>          cg_registry;
    spl::shared_ptr<const core::frame_producer_registry> producer_registry;
    spl::shared_ptr<const core::frame_consumer_registry> consumer_registry;
    std::weak_ptr<accelerator::accelerator_device>       ogl_device;
    std::function<void(bool)>                            shutdown_server_now;
    std::string proxy_host = u8(caspar::env::properties().get(L"configuration.amcp.media-server.host", L"127.0.0.1"));
    std::string proxy_port = u8(caspar::env::properties().get(L"configuration.amcp.media-server.port", L"8000"));

    std::map<std::wstring, std::pair<amcp_command_func, int>> commands;
    std::map<std::wstring, std::pair<amcp_command_func, int>> channel_commands;

    impl(const spl::shared_ptr<core::cg_producer_registry>&          cg_registry,
         const spl::shared_ptr<const core::frame_producer_registry>& producer_registry,
         const spl::shared_ptr<const core::frame_consumer_registry>& consumer_registry,
         const std::weak_ptr<accelerator::accelerator_device>&       ogl_device,
         std::function<void(bool)>                                   shutdown_server_now)
        : cg_registry(cg_registry)
        , producer_registry(producer_registry)
        , consumer_registry(consumer_registry)
        , ogl_device(ogl_device)
        , shutdown_server_now(shutdown_server_now)
    {
    }

    void init(const std::vector<spl::shared_ptr<core::video_channel>>& video_channels)
    {
        int index = 0;
        for (const auto& channel : video_channels) {
            std::wstring lifecycle_key = L"lock" + std::to_wstring(index);
            this->channels.push_back(channel_context(channel, lifecycle_key));
            ++index;
        }
    }


    AMCPCommand::ptr_type create_command(const std::wstring&      name,
                                         const std::wstring&      request_id,
                                                                  IO::ClientInfoPtr        client,
                                                                  std::list<std::wstring>& tokens) const
    {
        command_context ctx(std::move(client),
                            channel_context(),
                            -1,
                            -1,
                            channels,
                            cg_registry,
                            producer_registry,
                            consumer_registry,
                            shutdown_server_now,
                            proxy_host,
                            proxy_port,
                            ogl_device);

        return find_command(commands, name, request_id, ctx, tokens);
    }

    AMCPCommand::ptr_type create_channel_command(const std::wstring&      name,
        const std::wstring& request_id,
                                                                          IO::ClientInfoPtr        client,
                                                                          unsigned int             channel_index,
                                                                          int                      layer_index,
                                                                          std::list<std::wstring>& tokens) const
    {
        auto channel = channels.at(channel_index);

        command_context ctx(std::move(client),
                            channel,
                            channel_index,
                            layer_index,
                            channels,
                            cg_registry,
                            producer_registry,
                            consumer_registry,
                            shutdown_server_now,
                            proxy_host,
                            proxy_port,
                            ogl_device);

        return find_command(channel_commands, name, request_id, ctx, tokens);
    }

    std::shared_ptr<AMCPCommand>
    parse_command(IO::ClientInfoPtr client, std::list<std::wstring> tokens, const std::wstring& request_id) const
    {
        // Consume command name
        const std::basic_string<wchar_t> command_name = boost::to_upper_copy(tokens.front());
        tokens.pop_front();

        // Determine whether the next parameter is a channel spec or not
        int          channel_index = -1;
        int          layer_index   = -1;
        std::wstring channel_spec;
        parse_channel_id(tokens, channel_spec, channel_index, layer_index);

        // Create command instance
        std::shared_ptr<AMCPCommand> command;
        if (channel_index >= 0) {
            command = create_channel_command(command_name, request_id, client, channel_index, layer_index, tokens);

            if (!command) // Might be a non channel command, although the first argument is numeric
            {
                // Restore backed up channel spec string.
                tokens.push_front(channel_spec);
            }
        }

        // Create global instance
        if (!command) {
            command = create_command(command_name, request_id, client, tokens);
        }

        if (command && command->parameters().size() < command->minimum_parameters()) {
            CASPAR_LOG(error) << "Not enough parameters in command: " << command_name;
            return nullptr;
        }

        return std::move(command);
    }

    bool check_channel_lock(IO::ClientInfoPtr client, int channel_index) const
    {
        if (channel_index < 0)
            return true;

        auto lock = channels.at(channel_index).lock;
        return !(lock && !lock->check_access(client));
    }
};

amcp_command_repository::amcp_command_repository(
    const spl::shared_ptr<core::cg_producer_registry>&          cg_registry,
    const spl::shared_ptr<const core::frame_producer_registry>& producer_registry,
    const spl::shared_ptr<const core::frame_consumer_registry>& consumer_registry,
    const std::weak_ptr<accelerator::accelerator_device>&       ogl_device,
    std::function<void(bool)>                                   shutdown_server_now)
    : impl_(new impl(cg_registry, producer_registry, consumer_registry, ogl_device, shutdown_server_now))
{
}

void amcp_command_repository::init(const std::vector<spl::shared_ptr<core::video_channel>>& channels)
{
    impl_->init(channels);
}

AMCPCommand::ptr_type amcp_command_repository::create_command(const std::wstring&      name,
                                                              const std::wstring&      request_id,
                                                              IO::ClientInfoPtr        client,
                                                              std::list<std::wstring>& tokens) const
{
    return impl_->create_command(name, request_id, client, tokens);
}

const std::vector<channel_context>& amcp_command_repository::channels() const { return impl_->channels; }

std::shared_ptr<AMCPCommand> amcp_command_repository::parse_command(IO::ClientInfoPtr       client,
                                                                    std::list<std::wstring> tokens,
                                                                    const std::wstring&     request_id) const
{
    return impl_->parse_command(client, tokens, request_id);
}

bool amcp_command_repository::check_channel_lock(IO::ClientInfoPtr client, int channel_index) const
{
    return impl_->check_channel_lock(client, channel_index);
}

AMCPCommand::ptr_type amcp_command_repository::create_channel_command(const std::wstring&      name,
                                                                      const std::wstring&      request_id,
                                                                      IO::ClientInfoPtr        client,
                                                                      unsigned int             channel_index,
                                                                      int                      layer_index,
                                                                      std::list<std::wstring>& tokens) const
{
    return impl_->create_channel_command(name, request_id, client, channel_index, layer_index, tokens);
}

void amcp_command_repository::register_command(std::wstring      category,
                                               std::wstring      name,
                                               amcp_command_func command,
                                               int               min_num_params)
{
    impl_->commands.insert(std::make_pair(std::move(name), std::make_pair(std::move(command), min_num_params)));
}

void amcp_command_repository::register_channel_command(std::wstring      category,
                                                       std::wstring      name,
                                                       amcp_command_func command,
                                                       int               min_num_params)
{
    impl_->channel_commands.insert(std::make_pair(std::move(name), std::make_pair(std::move(command), min_num_params)));
}

}}} // namespace caspar::protocol::amcp
