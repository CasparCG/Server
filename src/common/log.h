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
 * Author: Robert Nagy, ronag89@gmail.com
 */

#pragma once

#include "utf.h"

#include <boost/log/sources/global_logger_storage.hpp>
#include <boost/log/sources/severity_feature.hpp>
#include <boost/log/sources/severity_logger.hpp>
#include <boost/log/trivial.hpp>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <boost/stacktrace.hpp>

#include <atomic>
#include <string>

namespace caspar { namespace log {

template <typename T>
void replace_nonprintable(std::basic_string<T, std::char_traits<T>, std::allocator<T>>& str, T with)
{
    std::locale loc(""); // Use system locale
    std::replace_if(
        str.begin(), str.end(), [&](T c) -> bool { return (!std::isprint(c, loc) && c != '\r' && c != '\n'); }, with);
}

template <typename T>
std::basic_string<T> replace_nonprintable_copy(std::basic_string<T, std::char_traits<T>, std::allocator<T>> str, T with)
{
    replace_nonprintable(str, with);
    return str;
}

std::string current_exception_diagnostic_information();

using caspar_logger = boost::log::sources::wseverity_logger<boost::log::trivial::severity_level>;

BOOST_LOG_INLINE_GLOBAL_LOGGER_DEFAULT(logger, caspar_logger)
#define CASPAR_LOG(lvl) BOOST_LOG_SEV(::caspar::log::logger::get(), boost::log::trivial::severity_level::lvl)

struct logging_config
{
    std::atomic<bool> align_columns = {false};
    std::wstring      current_level;
};

void          add_file_sink(const std::wstring& file);
void          add_cout_sink();
bool          set_log_level(const std::wstring& lvl);
std::wstring& get_log_level();
void          set_log_column_alignment(bool align_columns);

// Symbolizes the current call stack via boost::stacktrace. On MSVC this drives the dbgeng debug engine,
// which attaches to the process and opens a handle to every thread; doing this on every logged exception
// leaks Win32 thread handles under high exception churn (e.g. HLS reconnect storms). It is therefore NOT
// called on the exception-logging hot path below. Real crashes still get a full backtrace via the
// signal/terminate handler in main.cpp (safe_dump_to + from_dump). Keep this helper for explicit, manual
// diagnostic use only.
inline std::wstring get_stack_trace()
{
    auto bt = boost::stacktrace::stacktrace();
    if (bt) {
        return caspar::u16(boost::stacktrace::detail::to_string(&bt.as_vector()[0], bt.size()));
    }
    return L"";
}

#define CASPAR_LOG_CURRENT_EXCEPTION()                                                                                 \
    try {                                                                                                              \
        CASPAR_LOG(error) << L"Exception: "                                                                            \
                          << caspar::u16(::caspar::log::current_exception_diagnostic_information());                   \
    } catch (...) {                                                                                                    \
    }

#define CASPAR_LOG_CURRENT_CALL_STACK() // TODO (fix)

}} // namespace caspar::log
