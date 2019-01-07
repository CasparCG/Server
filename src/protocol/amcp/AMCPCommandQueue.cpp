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

#include "AMCPCommandQueue.h"

#include <common/future.h>
#include <common/timer.h>

#include <core/producer/stage.h>

#include <boost/lexical_cast.hpp>

namespace caspar { namespace protocol { namespace amcp {

AMCPCommandQueue::AMCPCommandQueue(const std::wstring& name, const std::vector<channel_context>& channels)
    : executor_(L"AMCPCommandQueue " + name)
    , channels_(channels)
{
}

AMCPCommandQueue::~AMCPCommandQueue() {}

std::future<bool>
exec_cmd(std::shared_ptr<AMCPCommand> cmd, const std::vector<channel_context>& channels, bool reply_without_req_id)
{
    try {
        try {
            caspar::timer timer;

            auto name = cmd->name();
            CASPAR_LOG(debug) << "Executing command: " << name;

            auto res = cmd->Execute(channels).share();
            return std::async(std::launch::async, [cmd, res, reply_without_req_id, timer, name]() -> bool {
                cmd->SendReply(res.get(), reply_without_req_id);

                CASPAR_LOG(debug) << "Executed command (" << timer.elapsed() << "s): " << name;
                return true;
            });

        } catch (file_not_found&) {
            CASPAR_LOG(error) << " File not found.";
            cmd->SendReply(L"404 " + cmd->name() + L" FAILED\r\n", reply_without_req_id);
        } catch (expected_user_error&) {
            cmd->SendReply(L"403 " + cmd->name() + L" FAILED\r\n", reply_without_req_id);
        } catch (user_error&) {
            CASPAR_LOG(error) << " Check syntax.";
            cmd->SendReply(L"403 " + cmd->name() + L" FAILED\r\n", reply_without_req_id);
        } catch (std::out_of_range&) {
            CASPAR_LOG(error) << L"Missing parameter. Check syntax.";
            cmd->SendReply(L"402 " + cmd->name() + L" FAILED\r\n", reply_without_req_id);
        } catch (boost::bad_lexical_cast&) {
            CASPAR_LOG(error) << L"Invalid parameter. Check syntax.";
            cmd->SendReply(L"403 " + cmd->name() + L" FAILED\r\n", reply_without_req_id);
        } catch (...) {
            CASPAR_LOG_CURRENT_EXCEPTION();
            CASPAR_LOG(error) << "Failed to execute command: " << cmd->name();
            cmd->SendReply(L"501 " + cmd->name() + L" FAILED\r\n", reply_without_req_id);
        }

    } catch (...) {
        CASPAR_LOG_CURRENT_EXCEPTION();
    }

    return make_ready_future(false);
}

void AMCPCommandQueue::AddCommand(std::shared_ptr<AMCPGroupCommand> pCurrentCommand)
{
    if (!pCurrentCommand)
        return;

    if (executor_.size() > 128) {
        try {
            CASPAR_LOG(error) << "AMCP Command Queue Overflow.";
            CASPAR_LOG(error) << "Failed to execute command:" << pCurrentCommand->name();
            pCurrentCommand->SendReply(L"504 QUEUE OVERFLOW\r\n");
        } catch (...) {
            CASPAR_LOG_CURRENT_EXCEPTION();
        }
        return;
    }

    executor_.begin_invoke([=] {
        try {
            Execute(pCurrentCommand);

            CASPAR_LOG(trace) << "Ready for a new command";
        } catch (...) {
            CASPAR_LOG_CURRENT_EXCEPTION();
        }
    });
}

void AMCPCommandQueue::Execute(std::shared_ptr<AMCPGroupCommand> cmd) const
{
    if (cmd->Commands().empty())
        return;

    // Shortcut for commands which are either not a batch, or don't need to be
    if (cmd->Commands().size() == 1) {
        exec_cmd(cmd->Commands().at(0), channels_, true);
        return;
    }

    caspar::timer timer;
    CASPAR_LOG(warning) << "Executing batch: " << cmd->name() << L"(" << cmd->Commands().size() << L" commands)";

    std::vector<channel_context>                      delayed_channels;
    std::vector<std::shared_ptr<core::stage_delayed>> delayed_stages;
    std::vector<std::future<bool>>                    results;
    std::vector<std::unique_lock<std::mutex>>         channel_locks;

    try {
        for (auto& ch : channels_) {
            std::promise<void> wa;

            auto st = std::make_shared<core::stage_delayed>(ch.raw_channel->stage(), ch.raw_channel->index());
            delayed_stages.push_back(st);
            delayed_channels.emplace_back(ch.raw_channel, st, ch.lifecycle_key_);
        }

        // 'execute' aka queue all comamnds
        for (auto& cmd2 : cmd->Commands()) {
            results.push_back(exec_cmd(cmd2, delayed_channels, cmd->HasClient()));
        }

        // lock all the channels needed
        for (auto& st : delayed_stages) {
            if (st->count_queued() == 0) {
                continue;
            }

            channel_locks.push_back(st->get_lock());
        }

        // execute the commands
        for (auto& st : delayed_stages) {
            st->release();
        }
    } catch (...) {
        // Ensure the created executors don't get leaked
        for (auto& st : delayed_stages) {
            st->abort();
        }

        throw;
    }

    // wait for the commands to finish
    for (auto& st : delayed_stages) {
        st->wait();
    }
    // TODO - it would be good to move the lock clearing into the executor, but that will cause issues with swap as they
    // will unlock early
    channel_locks.clear();

    int failed = 0;
    for (auto& f : results) {
        if (!f.get())
            failed++;
    }

    if (failed > 0)
        cmd->SendReply(L"202 COMMIT PARTIAL\r\n"); // TODO report failed count/info
    else
        cmd->SendReply(L"202 COMMIT OK\r\n");

    CASPAR_LOG(debug) << "Executed batch (" << timer.elapsed() << "s): " << cmd->name();
}

}}} // namespace caspar::protocol::amcp
