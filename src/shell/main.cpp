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

// tbbmalloc_proxy:
// Replace the standard memory allocation routines in Microsoft* C/C++ RTL
// (malloc/free, global new/delete, etc.) with the TBB memory allocator.
#include <tbb/task_scheduler_init.h>

#if defined _DEBUG && defined _MSC_VER
#define _CRTDBG_MAP_ALLOC
#include <crtdbg.h>
#include <stdlib.h>
#else
#include <tbb/tbbmalloc_proxy.h>
#endif

#include "included_modules.h"
#include "platform_specific.h"
#include "server.h"

#include <protocol/amcp/AMCPProtocolStrategy.h>
#include <protocol/util/strategy_adapters.h>

#include <common/env.h>
#include <common/except.h>
#include <common/gl/gl_check.h>
#include <common/log.h>

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/filesystem.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/locale.hpp>
#include <boost/property_tree/detail/file_parser_error.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <boost/stacktrace.hpp>

#include <atomic>
#include <future>
#include <set>

#include <clocale>
#include <csignal>

namespace caspar {

void setup_global_locale()
{
    boost::locale::generator gen;
    gen.categories(boost::locale::codepage_facet);

    std::locale::global(gen(""));

    // sscanf is used in for example FFmpeg where we want decimals to be parsed as .
    std::setlocale(LC_ALL, "C");
}

void print_info()
{
    CASPAR_LOG(info) << L"############################################################################";
    CASPAR_LOG(info) << L"CasparCG Server is distributed by the Swedish Broadcasting Corporation (SVT)";
    CASPAR_LOG(info) << L"under the GNU General Public License GPLv3 or higher.";
    CASPAR_LOG(info) << L"Please see LICENSE.TXT for details.";
    CASPAR_LOG(info) << L"http://www.casparcg.com/";
    CASPAR_LOG(info) << L"############################################################################";
    CASPAR_LOG(info) << L"Starting CasparCG Video and Graphics Playout Server " << env::version();
}

auto run(const std::wstring& config_file_name, std::atomic<bool>& should_wait_for_keypress)
{
    auto promise  = std::make_shared<std::promise<bool>>();
    auto future   = promise->get_future();
    auto shutdown = [promise = std::move(promise)](bool restart) { promise->set_value(restart); };

    print_info();

    // Create server object which initializes channels, protocols and controllers.
    std::unique_ptr<server> caspar_server(new server(shutdown));

    // For example CEF resets the global locale, so this is to reset it back to "our" preference.
    setup_global_locale();

    std::wstringstream                                      str;
    boost::property_tree::xml_writer_settings<std::wstring> w(' ', 3);
    boost::property_tree::write_xml(str, env::properties(), w);
    CASPAR_LOG(info) << boost::filesystem::canonical(config_file_name)
                     << L":\n-----------------------------------------\n"
                     << str.str() << L"-----------------------------------------";

    caspar_server->start();

    // Create a dummy client which prints amcp responses to console.
    auto console_client = spl::make_shared<IO::ConsoleClientInfo>();

    auto amcp = protocol::amcp::create_wchar_amcp_strategy_factory(L"Console",
                                                                   caspar_server->get_amcp_command_repository(),
                                                                   caspar_server->get_amcp_command_scheduler())
                    ->create(console_client);

    // Use separate thread for the blocking console input, will be terminated
    // anyway when the main thread terminates.
    std::thread([&]() mutable {
        std::wstring wcmd;
        while (true) {
#ifdef WIN32
            if (!std::getline(std::wcin, wcmd)) { // TODO: It's blocking...
                std::wcin.clear();
                continue;
            }
#else
            // Linux gets stuck in an endless loop if wcin gets a multibyte utf8 char
            std::string cmd1;
            if (!std::getline(std::cin, cmd1)) { // TODO: It's blocking...
                std::cin.clear();
                continue;
            }
            wcmd = u16(cmd1);
#endif

            // If the cmd is empty, no point trying to parse it
            if (!wcmd.empty()) {
                if (boost::iequals(wcmd, L"EXIT") || boost::iequals(wcmd, L"Q") || boost::iequals(wcmd, L"QUIT") ||
                    boost::iequals(wcmd, L"BYE")) {
                    CASPAR_LOG(info) << L"Received message from Console: " << wcmd << L"\\r\\n";
                    should_wait_for_keypress = true;
                    shutdown(false); // false to not restart
                    break;
                }

                wcmd += L"\r\n";
                amcp->parse(wcmd);
            }
        }
    })
        .detach();

    future.wait();

    caspar_server.reset();

    return future.get();
}

void signal_handler(int signum)
{
    ::signal(signum, SIG_DFL);
    boost::stacktrace::safe_dump_to("./backtrace.dump");
    ::raise(SIGABRT);
}

void terminate_handler()
{
    try {
        std::rethrow_exception(std::current_exception());
    } catch (...) {
        CASPAR_LOG_CURRENT_EXCEPTION();
    }
    std::abort();
}

} // namespace caspar

int main(int argc, char** argv)
{
    using namespace caspar;

    if (intercept_command_line_args(argc, argv))
        return 0;

    ::signal(SIGSEGV, signal_handler);
    ::signal(SIGABRT, signal_handler);
    std::set_terminate(caspar::terminate_handler);

    static auto backtrace = "./backtrace.dump";
    if (boost::filesystem::exists(backtrace)) {
        std::ifstream     ifs(backtrace);
        std::stringstream str;
        str << boost::stacktrace::stacktrace::from_dump(ifs);
        CASPAR_LOG(error) << u16(str.str());
        ifs.close();
        boost::filesystem::remove(backtrace);
    }

    int return_code = 0;
    setup_prerequisites();

    setup_global_locale();

    std::wcout << L"Type \"q\" to close application." << std::endl;

    // Set debug mode.
    auto debugging_environment = setup_debugging_environment();

    // Increase process priority.
    increase_process_priority();

    tbb::task_scheduler_init init;
    std::wstring             config_file_name(L"casparcg.config");

    try {
        // Configure environment properties from configuration.
        if (argc >= 2)
            config_file_name = caspar::u16(argv[1]);

        log::add_cout_sink();
        env::configure(config_file_name);

        {
            std::wstring target_level = env::properties().get(L"configuration.log-level", L"info");
            if (!log::set_log_level(target_level)) {
                log::set_log_level(L"info");
                std::wcout << L"Failed to set log level [" << target_level << L"]" << std::endl;
            }
        }

        if (env::properties().get(L"configuration.debugging.remote", false))
            wait_for_remote_debugging();

        // Start logging to file.
        log::add_file_sink(env::log_folder() + L"caspar");
        std::wcout << L"Logging [" << log::get_log_level() << L"] or higher severity to " << env::log_folder()
                   << std::endl
                   << std::endl;

        // Once logging to file, log configuration warnings.
        env::log_configuration_warnings();

        // Setup console window.
        setup_console_window();

        std::atomic<bool> should_wait_for_keypress;
        should_wait_for_keypress = false;
        auto should_restart      = run(config_file_name, should_wait_for_keypress);
        return_code              = should_restart ? 5 : 0;

        CASPAR_LOG(info) << "Successfully shutdown CasparCG Server.";

        if (should_wait_for_keypress)
            wait_for_keypress();
    } catch (boost::property_tree::file_parser_error& e) {
        CASPAR_LOG(fatal) << "At " << u8(config_file_name) << ":" << e.line() << ": " << e.message()
                          << ". Please check the configuration file (" << u8(config_file_name) << ") for errors.";
        wait_for_keypress();
    } catch (user_error&) {
        CASPAR_LOG_CURRENT_EXCEPTION();
        CASPAR_LOG(fatal) << " Please check the configuration file (" << u8(config_file_name) << ") for errors.";
        wait_for_keypress();
    } catch (...) {
        CASPAR_LOG_CURRENT_EXCEPTION();
        CASPAR_LOG(fatal) << L"Unhandled exception in main thread. Please report this error on the GitHub project page "
                             L"(www.github.com/casparcg/server/issues).";
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        std::wcout << L"\n\nCasparCG will automatically shutdown. See the log file located at the configured log-file "
                      L"folder for more information.\n\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(4000));
    }

    return return_code;
}
