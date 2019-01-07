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
#include "AMCPCommandScheduler.h"
#include "AMCPProtocolStrategy.h"
#include "amcp_command_context.h"
#include "amcp_command_repository.h"
#include "amcp_shared.h"
#include "protocol/util/strategy_adapters.h"
#include "protocol/util/tokenize.h"

#include <algorithm>
#include <cctype>
#include <future>

#include <boost/algorithm/string.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/log/keywords/delimiter.hpp>

#include <common/diagnostics/graph.h>

#if defined(_MSC_VER)
#pragma warning(push, 1) // TODO: Legacy code, just disable warnings
#endif

namespace caspar { namespace protocol { namespace amcp {

using IO::ClientInfoPtr;

class AMCPClientBatchInfo
{
  public:
    AMCPClientBatchInfo(const IO::client_connection<wchar_t>::ptr client)
        : client_(client)
        , commands_()
        , in_progress_(false)
    {
    }
    ~AMCPClientBatchInfo() { commands_.clear(); }

    void add_command(const AMCPCommand::ptr_type cmd) { commands_.push_back(cmd); }

    std::shared_ptr<AMCPGroupCommand> finish()
    {
        const auto res = std::make_shared<AMCPGroupCommand>(commands_, client_, request_id_);

        in_progress_ = false;
        commands_.clear();

        return std::move(res);
    }

    void begin(const std::wstring& request_id)
    {
        request_id_  = request_id;
        in_progress_ = true;
        commands_.clear();
    }

    bool                in_progress() const { return in_progress_; }
    const std::wstring& request_id() const { return request_id_; }

  private:
    const IO::client_connection<wchar_t>::ptr client_;
    std::vector<AMCPCommand::ptr_type>        commands_;
    bool                                      in_progress_;
    std::wstring                              request_id_;
};

class AMCPProtocolStrategy
{
  private:
    std::vector<AMCPCommandQueue::ptr_type>  commandQueues_;
    spl::shared_ptr<amcp_command_repository> repo_;
    spl::shared_ptr<AMCPCommandScheduler>    scheduler_;
    std::vector<std::shared_ptr<void>>       schedule_ops_;

  public:
    AMCPProtocolStrategy(const std::wstring&                             name,
                         const spl::shared_ptr<amcp_command_repository>& repo,
                         const spl::shared_ptr<AMCPCommandScheduler>&    scheduler)
        : repo_(repo)
        , scheduler_(scheduler)
    {
        commandQueues_.push_back(spl::make_shared<AMCPCommandQueue>(L"General Queue for " + name, repo_->channels()));

        scheduler_->create_channels(static_cast<int>(repo_->channels().size()));

        int i = 0;
        for (const auto& ch : repo_->channels()) {
            auto queue = spl::make_shared<AMCPCommandQueue>(
                L"Channel " + boost::lexical_cast<std::wstring>(i + 1) + L" for " + name, repo_->channels());
            std::weak_ptr<AMCPCommandQueue> queue_weak = queue;
            schedule_ops_.push_back(ch.raw_channel->add_timecode_listener(
                [&, i, queue_weak](core::frame_timecode timecode, spl::shared_ptr<caspar::diagnostics::graph> graph) {
                    auto queue2 = queue_weak.lock();
                    if (!queue2)
                        return;

                    const auto cmds = scheduler_->schedule(i, timecode);
                    if (!cmds) {
                        graph->set_tag(caspar::diagnostics::tag_severity::WARNING, "skipped-schedule");
                        return;
                    }

                    for (auto cmd : cmds.get()) {
                        queue2->AddCommand(cmd);
                    }
                }));
            commandQueues_.push_back(queue);
            i++;
        }
    }

    ~AMCPProtocolStrategy() {}

    enum class error_state
    {
        no_error = 0,
        command_error,
        channel_error,
        parameters_error,
        unknown_error,
        access_error
    };

    // The parser method expects message to be complete messages with the delimiter stripped away.
    // Thesefore the AMCPProtocolStrategy should be decorated with a delimiter_based_chunking_strategy
    void parse(const std::wstring& message, ClientInfoPtr client, const std::shared_ptr<AMCPClientBatchInfo>& batch)
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
        error_state  err = parse_command_string(client, batch, tokens, request_id, command_name);
        if (err != error_state::no_error) {
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
                                                       boost::lexical_cast<std::wstring>(static_cast<int>(err))));
            }
            client->send(answer.str());
        }
    }

  private:
    error_state parse_command_string(ClientInfoPtr                               client,
                                     const std::shared_ptr<AMCPClientBatchInfo>& batch,
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

            if (parse_batch_commands(batch, tokens, request_id, error)) {
                return error;
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

            if (batch->in_progress()) {
                batch->add_command(command);
                return error_state::no_error;
            }

            auto wrapped = std::make_shared<AMCPGroupCommand>(command);
            commandQueues_.at(channel_index + 1)->AddCommand(std::move(wrapped));
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

    bool parse_batch_commands(const std::shared_ptr<AMCPClientBatchInfo>& batch,
                              std::list<std::wstring>&                    tokens,
                              std::wstring&                               request_id,
                              error_state&                                error)
    {
        if (boost::iequals(tokens.front(), L"COMMIT")) {
            if (!batch->in_progress()) {
                error = error_state::command_error;
                return true;
            }

            commandQueues_.at(0)->AddCommand(std::move(batch->finish()));
            error = error_state::no_error;
            return true;
        }
        if (boost::iequals(tokens.front(), L"BEGIN")) {
            if (batch->in_progress()) {
                error = error_state::command_error;
                return true;
            }

            batch->begin(request_id);
            error = error_state::no_error;
            return true;
        }
        if (boost::iequals(tokens.front(), L"DISCARD")) {
            if (!batch->in_progress()) {
                error = error_state::command_error;
                return true;
            }

            batch->finish();
            error = error_state::no_error;
            return true;
        }

        return false;
    }
};

class AMCPClientStrategy : public IO::protocol_strategy<wchar_t>
{
    const std::shared_ptr<AMCPProtocolStrategy> strategy_;
    const std::shared_ptr<AMCPClientBatchInfo>  batch_;
    ClientInfoPtr                               client_info_;

  public:
    AMCPClientStrategy(const std::shared_ptr<AMCPProtocolStrategy>& strategy,
                       const IO::client_connection<wchar_t>::ptr&   client_connection)
        : strategy_(strategy)
        , batch_(std::make_shared<AMCPClientBatchInfo>(client_connection))
        , client_info_(client_connection)
    {
    }

    void parse(const std::basic_string<wchar_t>& data) override { strategy_->parse(data, client_info_, batch_); }
};

class amcp_client_strategy_factory : public IO::protocol_strategy_factory<wchar_t>
{
  public:
    amcp_client_strategy_factory(const std::shared_ptr<AMCPProtocolStrategy> strategy)
        : strategy_(strategy)
    {
    }

    IO::protocol_strategy<wchar_t>::ptr create(const IO::client_connection<wchar_t>::ptr& client_connection) override
    {
        return spl::make_shared<AMCPClientStrategy>(strategy_, client_connection);
    }

  private:
    const std::shared_ptr<AMCPProtocolStrategy> strategy_;
};

IO::protocol_strategy_factory<char>::ptr
create_char_amcp_strategy_factory(const std::wstring&                             name,
                                  const spl::shared_ptr<amcp_command_repository>& repo,
                                  const spl::shared_ptr<AMCPCommandScheduler>&    scheduler)
{
    auto amcp_strategy = spl::make_shared<AMCPProtocolStrategy>(std::move(name), repo, scheduler);
    auto amcp_client   = spl::make_shared<amcp_client_strategy_factory>(amcp_strategy);
    auto to_unicode    = spl::make_shared<IO::to_unicode_adapter_factory>("UTF-8", amcp_client);
    return spl::make_shared<IO::delimiter_based_chunking_strategy_factory<char>>("\r\n", to_unicode);
}

IO::protocol_strategy_factory<wchar_t>::ptr
create_wchar_amcp_strategy_factory(const std::wstring&                             name,
                                   const spl::shared_ptr<amcp_command_repository>& repo,
                                   const spl::shared_ptr<AMCPCommandScheduler>&    scheduler)
{
    auto amcp_strategy = spl::make_shared<AMCPProtocolStrategy>(std::move(name), repo, scheduler);
    auto amcp_client   = spl::make_shared<amcp_client_strategy_factory>(amcp_strategy);
    return spl::make_shared<IO::delimiter_based_chunking_strategy_factory<wchar_t>>(L"\r\n", amcp_client);
}

}}} // namespace caspar::protocol::amcp
