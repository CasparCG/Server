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

#include "../StdAfx.h"

#include "AMCPCommand.h"
#include "AMCPCommandQueue.h"
#include "AMCPProtocolStrategy.h"
#include "amcp_command_repository.h"
#include "amcp_shared.h"

#include "../util/tokenize.h"

#include <algorithm>

#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/lexical_cast.hpp>

#if defined(_MSC_VER)
#pragma warning(push, 1) // TODO: Legacy code, just disable warnings
#endif

namespace caspar { namespace protocol { namespace amcp {

using IO::ClientInfoPtr;

template <typename Out, typename In>
bool try_lexical_cast(const In& input, Out& result)
{
    Out  saved   = result;
    bool success = boost::conversion::detail::try_lexical_convert(input, result);

    if (!success)
        result = saved; // Needed because of how try_lexical_convert is implemented.

    return success;
}

struct AMCPProtocolStrategy::impl
{
  private:
    std::vector<AMCPCommandQueue::ptr_type>  commandQueues_;
    spl::shared_ptr<amcp_command_repository> repo_;

  public:
    impl(const std::wstring& name, const spl::shared_ptr<amcp_command_repository>& repo)
        : repo_(repo)
    {
        commandQueues_.push_back(spl::make_shared<AMCPCommandQueue>(L"General Queue for " + name));

        for (int i = 0; i < repo_->channels().size(); ++i) {
            commandQueues_.push_back(
                spl::make_shared<AMCPCommandQueue>(L"Channel " + std::to_wstring(i + 1) + L" for " + name));
        }
    }

    ~impl() {}

    enum class error_state
    {
        no_error = 0,
        command_error,
        channel_error,
        parameters_error,
        unknown_error,
        access_error
    };

    struct command_interpreter_result
    {
        std::shared_ptr<caspar::IO::lock_container> lock;
        std::wstring                                request_id;
        std::wstring                                command_name;
        AMCPCommand::ptr_type                       command;
        error_state                                 error = error_state::no_error;
        std::shared_ptr<AMCPCommandQueue>           queue;
    };

    // The paser method expects message to be complete messages with the delimiter stripped away.
    // Thesefore the AMCPProtocolStrategy should be decorated with a delimiter_based_chunking_strategy
    void Parse(const std::wstring& message, ClientInfoPtr client)
    {
        std::list<std::wstring> tokens;
        IO::tokenize(message, tokens);

        if (!tokens.empty() && boost::iequals(tokens.front(), L"PING")) {
            tokens.pop_front();
            std::wstringstream answer;
            answer << L"PONG";

            for (auto t : tokens)
                answer << L" " << t;

            answer << "\r\n";
            client->send(answer.str(), true);
            return;
        }

        CASPAR_LOG(info) << L"Received message from " << client->address() << ": " << message << L"\\r\\n";

        std::wstring request_id;
        std::wstring command_name;
        error_state err = parse_command_string(client, tokens, request_id, command_name);
        if (err!= error_state::no_error) {
            std::wstringstream answer;

            if (!request_id.empty())
                answer << L"RES " << request_id << L" ";

            switch (err) {
                case error_state::command_error:
                    answer << L"400 ERROR\r\n" << message << "\r\n";
                    break;
                case error_state::channel_error:
                    answer << L"401 " << command_name << " ERROR\r\n";
                    break;
                case error_state::parameters_error:
                    answer << L"402 " << command_name << " ERROR\r\n";
                    break;
                case error_state::access_error:
                    answer << L"503 " << command_name << " FAILED\r\n";
                    break;
                case error_state::unknown_error:
                    answer << L"500 FAILED\r\n";
                    break;
                default:
                    CASPAR_THROW_EXCEPTION(programming_error()
                                           << msg_info(L"Unhandled error_state enum constant " +
                                                       std::to_wstring(static_cast<int>(err))));
            }
            client->send(answer.str());
        }
    }

  private:
    error_state parse_command_string(ClientInfoPtr                               client,
                                     std::list<std::wstring>                     tokens,
                                     std::wstring&                               request_id,
                                     std::wstring&                               command_name)
    {
        try {
            // Discard GetSwitch
            if (!tokens.empty() && tokens.front().at(0) == L'/')
                tokens.pop_front();

            error_state error = parse_request_token(tokens, request_id);
            if (error != error_state::no_error) {
                return error;
            }

            // Fail if no more tokens.
            if (tokens.empty()) {
                return error_state::command_error;
            }

            command_name                               = boost::to_upper_copy(tokens.front());
            const std::shared_ptr<AMCPCommand> command = repo_->parse_command(client, tokens, request_id);
            if (!command) {
                return error_state::command_error;
            }

            const int channel_index = command->channel_index();
            if (!repo_->check_channel_lock(client, channel_index)) {
                return error_state::access_error;
            }

            commandQueues_.at(channel_index + 1)->AddCommand(std::move(command));
            return error_state::no_error;

        } catch (std::out_of_range&) {
            CASPAR_LOG(error) << "Invalid channel specified.";
            return error_state::channel_error;
        } catch (...) {
            CASPAR_LOG_CURRENT_EXCEPTION();
            return error_state::unknown_error;
        }
    }

    static error_state parse_request_token(std::list<std::wstring>& tokens, std::wstring& request_id)
    {
        if (tokens.empty() || !boost::iequals(tokens.front(), L"REQ")) {
            return error_state::no_error;
        }

        tokens.pop_front();

        if (tokens.empty()) {
            return error_state::parameters_error;
        }

        request_id = std::move(tokens.front());
        tokens.pop_front();

        return error_state::no_error;
    }
};

AMCPProtocolStrategy::AMCPProtocolStrategy(const std::wstring&                             name,
                                           const spl::shared_ptr<amcp_command_repository>& repo)
    : impl_(spl::make_unique<impl>(name, repo))
{
}
AMCPProtocolStrategy::~AMCPProtocolStrategy() {}
void AMCPProtocolStrategy::Parse(const std::wstring& msg, IO::ClientInfoPtr pClientInfo)
{
    impl_->Parse(msg, pClientInfo);
}

}}} // namespace caspar::protocol::amcp
