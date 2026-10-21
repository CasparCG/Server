/*
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
 */
#pragma once

#include "../interop/libomt.h"

#include <protocol/amcp/amcp_command_context.h>

#include <mutex>
#include <string>
#include <vector>

namespace caspar { namespace omt {

// The part of the libomt API that CasparCG uses. The functions are looked up when libomt is
// loaded, so CasparCG builds and starts without libomt being installed.
struct omt_lib
{
    decltype(&::omt_discovery_getaddresses) discovery_getaddresses;

    decltype(&::omt_receive_create)  receive_create;
    decltype(&::omt_receive_destroy) receive_destroy;
    decltype(&::omt_receive)         receive;

    decltype(&::omt_send_create)      send_create;
    decltype(&::omt_send_destroy)     send_destroy;
    decltype(&::omt_send)             send;
    decltype(&::omt_send_getaddress)  send_getaddress;
    decltype(&::omt_send_connections) send_connections;

    // Stops the threads that libomt runs on its own. Not every libomt has it.
    decltype(&::omt_shutdown) shutdown;
};

// Loads libomt on the first call and returns it on every call.
// Throws caspar::not_supported if libomt is missing or does not export everything in omt_lib.
omt_lib* load_library();

// Returns a lock that must be held while creating or destroying a sender or receiver.
// Calls are serialized across the whole process and kept at least 300 ms apart, because libomt
// loses the upper/lower case of the discovery name of every sender after the first one.
std::unique_lock<std::mutex> serialize_create_call();

// Converts a name to ASCII for use as a sender name: diacritics are removed and any other
// non-ASCII character becomes '_'. libomt breaks discovery of a sender with a non-ASCII name
// as soon as there is more than one sender in the process.
// Receivers are not affected, they connect with the name exactly as given because it has to
// match what the remote sender announces.
std::wstring make_ascii_safe_name(const std::wstring& name);

// Returns the sources currently found by discovery, as "HOSTNAME (Source Name)" strings.
std::vector<std::string> get_current_sources();

// AMCP "OMT LIST" command.
std::wstring list_command(protocol::amcp::command_context& ctx);

}} // namespace caspar::omt
