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

#include <boost/lexical_cast.hpp>
#include <common/except.h>
#include <common/timer.h>

namespace caspar { namespace protocol { namespace amcp {

namespace {

std::mutex& get_global_mutex()
{
    static std::mutex mutex;

    return mutex;
}

std::map<std::wstring, AMCPCommandQueue*>& get_instances()
{
    static std::map<std::wstring, AMCPCommandQueue*> queues;

    return queues;
}

} // namespace

AMCPCommandQueue::AMCPCommandQueue(const std::wstring& name)
    : executor_(L"AMCPCommandQueue " + name)
{
    std::lock_guard<std::mutex> lock(get_global_mutex());

    get_instances().insert(std::make_pair(name, this));
}

AMCPCommandQueue::~AMCPCommandQueue()
{
    std::lock_guard<std::mutex> lock(get_global_mutex());

    get_instances().erase(executor_.name());
}

void AMCPCommandQueue::AddCommand(AMCPCommand::ptr_type pCurrentCommand)
{
    if (!pCurrentCommand)
        return;

    if (executor_.size() > 128) {
        try {
            CASPAR_LOG(error) << "AMCP Command Queue Overflow.";
            CASPAR_LOG(error) << "Failed to execute command:" << pCurrentCommand->print();
            pCurrentCommand->SendReply(L"504 QUEUE OVERFLOW\r\n");
        } catch (...) {
            CASPAR_LOG_CURRENT_EXCEPTION();
        }
        return;
    }

    executor_.begin_invoke([=] {
        try {
            try {
                caspar::timer timer;

                auto print  = pCurrentCommand->print();
                auto params = boost::join(pCurrentCommand->parameters(), L" ");

                CASPAR_LOG(debug) << "Executing command: " << print;

                auto res = pCurrentCommand->Execute();
                CASPAR_LOG(debug) << "Executed command (" << timer.elapsed() << "s): " << print;
                pCurrentCommand->SendReply(res);
            } catch (file_not_found&) {
                CASPAR_LOG(error) << " Turn on log level debug for stacktrace.";
                pCurrentCommand->SendReply(L"404 " + pCurrentCommand->print() + L" FAILED\r\n");
            } catch (expected_user_error&) {
                pCurrentCommand->SendReply(L"403 " + pCurrentCommand->print() + L" FAILED\r\n");
            } catch (user_error&) {
                CASPAR_LOG(error) << " Check syntax. Turn on log level debug for stacktrace.";
                pCurrentCommand->SendReply(L"403 " + pCurrentCommand->print() + L" FAILED\r\n");
            } catch (std::out_of_range&) {
                CASPAR_LOG(error) << L"Missing parameter. Check syntax. Turn on log level debug for stacktrace.";
                pCurrentCommand->SendReply(L"402 " + pCurrentCommand->print() + L" FAILED\r\n");
            } catch (boost::bad_lexical_cast&) {
                CASPAR_LOG(error) << L"Invalid parameter. Check syntax. Turn on log level debug for stacktrace.";
                pCurrentCommand->SendReply(L"403 " + pCurrentCommand->print() + L" FAILED\r\n");
            } catch (...) {
                CASPAR_LOG_CURRENT_EXCEPTION();
                CASPAR_LOG(error) << "Failed to execute command: " << pCurrentCommand->print();
                pCurrentCommand->SendReply(L"501 " + pCurrentCommand->print() + L" FAILED\r\n");
            }

            CASPAR_LOG(trace) << "Ready for a new command";
        } catch (...) {
            CASPAR_LOG_CURRENT_EXCEPTION();
        }
    });
}

}}} // namespace caspar::protocol::amcp
