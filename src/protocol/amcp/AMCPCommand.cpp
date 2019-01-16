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

#include "../StdAfx.h"

#include "AMCPCommand.h"
#include <core/producer/stage.h>

namespace caspar { namespace protocol { namespace amcp {

std::future<std::wstring> AMCPCommand::Execute(const std::vector<channel_context>& channels)
{
    return command_(ctx_, channels);
}

void send_reply(IO::ClientInfoPtrStd client, const std::wstring& str, const std::wstring& request_id)
{
    if (str.empty())
        return;

    std::wstring reply = str;
    if (!request_id.empty())
        reply = L"RES " + request_id + L" " + str;

    client->send(std::move(reply));
}

void AMCPCommand::SendReply(const std::wstring& str, bool reply_without_req_id) const
{
    if (reply_without_req_id || !request_id_.empty()) {
        send_reply(ctx_.client, str, request_id_);
    }
}

void AMCPGroupCommand::SendReply(const std::wstring& str) const
{
    if (client_) {
        send_reply(client_, str, request_id_);
        return;
    }

    if (commands_.size() == 1) {
        commands_.at(0)->SendReply(str, true);
    }
}

std::wstring AMCPGroupCommand::name() const
{
    if (commands_.size() == 1) {
        return commands_.at(0)->name();
    }

    return L"BATCH";
}

}}} // namespace caspar::protocol::amcp
