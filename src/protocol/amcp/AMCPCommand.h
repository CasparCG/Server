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

namespace caspar { namespace protocol { namespace amcp {

class AMCPCommand
{
  private:
    const command_context_simple ctx_;
    const amcp_command_func      command_;
    const std::wstring           name_;
    const std::wstring           request_id_;

  public:
    AMCPCommand(const command_context_simple& ctx,
                const amcp_command_func&      command,
                const std::wstring&           name,
                const std::wstring&           request_id)

        : ctx_(ctx)
        , command_(command)
        , name_(name)
        , request_id_(request_id)
    {
    }

    typedef std::shared_ptr<AMCPCommand> ptr_type;

    std::future<std::wstring> Execute(const std::vector<channel_context>& channels);

    void SendReply(const std::wstring& str, bool reply_without_req_id) const;

    std::wstring name() const { return name_; }

    int channel_index() const { return ctx_.channel_index; }
};

class AMCPGroupCommand
{
    const std::vector<std::shared_ptr<AMCPCommand>> commands_;
    const IO::ClientInfoPtrStd                      client_;
    const std::wstring                              request_id_;
    const bool                                      is_batch_;

  public:
    AMCPGroupCommand(const std::vector<std::shared_ptr<AMCPCommand>> commands,
                     IO::ClientInfoPtrStd                            client,
                     const std::wstring&                             request_id)
        : commands_(commands)
        , client_(client)
        , request_id_(request_id)
        , is_batch_(true)
    {
    }

    AMCPGroupCommand(const std::vector<std::shared_ptr<AMCPCommand>> commands)
        : commands_(commands)
        , client_(nullptr)
        , request_id_(L"")
        , is_batch_(true)
    {
    }

    AMCPGroupCommand(const std::shared_ptr<AMCPCommand> command)
        : commands_({command})
        , client_(nullptr)
        , request_id_(L"")
        , is_batch_(false)
    {
    }

    bool HasClient() const { return !!client_; }

    void SendReply(const std::wstring& str) const;

    std::wstring name() const;

    std::vector<std::shared_ptr<AMCPCommand>> Commands() const { return commands_; }
};

}}} // namespace caspar::protocol::amcp
