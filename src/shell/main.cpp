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
// (malloc/free, global new/delete, etc.) with the TBB memory allocator
// as the default allocator suffers from low performance.

#if defined _DEBUG && defined _MSC_VER
#define _CRTDBG_MAP_ALLOC
#include <crtdbg.h>
#include <stdlib.h>
#elif defined _MSC_VER
#include <tbb/tbbmalloc_proxy.h>
#endif

#include "included_modules.h"
#include "platform_specific.h"
#include "server.h"

#include <protocol/amcp/AMCPProtocolStrategy.h>
#include <protocol/util/strategy_adapters.h>

#include <common/env.h>
#include <common/except.h>
#include <common/log.h>
#include <common/ptree.h>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/asio.hpp>
#include <boost/filesystem.hpp>
#include <boost/locale.hpp>
#include <boost/property_tree/detail/file_parser_error.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <boost/stacktrace.hpp>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>

#include <clocale>
#include <csignal>
#include <fcntl.h>

#ifdef WIN32
#include <io.h>
#else
#include <unistd.h>
#endif
#include <sys/stat.h>

namespace caspar {

std::string crash_dump_path = "./backtrace.dump";

void setup_global_locale()
{
    boost::locale::generator gen;
    gen.categories(boost::locale::category_t::codepage);

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
    boost::asio::io_context io;

    auto restart  = false;
    auto shutdown = [&](bool restart_) {
        restart = restart_;
        io.stop();
    };

    print_info();

    // Create server object which initializes channels, protocols and controllers.
    std::unique_ptr<server> caspar_server(new server(shutdown));

    // For example CEF resets the global locale, so this is to reset it back to "our" preference.
    setup_global_locale();

    std::wstringstream                                      str;
    boost::property_tree::xml_writer_settings<std::wstring> w(' ', 3);
    boost::property_tree::write_xml(str, env::properties(), w);
    CASPAR_LOG(info) << boost::filesystem::absolute(config_file_name).lexically_normal()
                     << L":\n-----------------------------------------\n"
                     << str.str() << L"-----------------------------------------";

    caspar_server->start();

    // Create a dummy client which prints amcp responses to console.
    auto console_client = spl::make_shared<IO::ConsoleClientInfo>();

    auto amcp =
        protocol::amcp::create_wchar_amcp_strategy_factory(L"Console", caspar_server->get_amcp_command_repository())
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
                if (std::cin.eof()) {
                    std::cin.clear();
                    break;
                }
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
    }).detach();

    // Signal handlers needs to be installed after Cef has been initialized.
    boost::asio::signal_set signals(io, SIGINT, SIGTERM);
    signals.async_wait([&](auto, auto) { io.stop(); });

    io.run();

    caspar_server.reset();

    return restart;
}

void signal_handler(int signum)
{
    ::signal(signum, SIG_DFL);
    // Prevent abort() from re-entering this handler.
    ::signal(SIGABRT, SIG_DFL);
    // Exclusive creation protects retained dumps, including hard-linked archives.
    constexpr char failure[] = "Could not create crash dump; existing files were not overwritten.\n";
#ifdef WIN32
    const auto fd = ::_open(crash_dump_path.c_str(), _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY, _S_IREAD | _S_IWRITE);
    if (fd != -1) {
        void* frames[129];
        const auto frame_count = boost::stacktrace::safe_dump_to(frames, sizeof(frames));
        const auto written = ::_write(fd, frames, static_cast<unsigned int>(frame_count * sizeof(frames[0])));
        (void)written;
        ::_close(fd);
    } else {
        const auto written = ::_write(2, failure, sizeof(failure) - 1);
        (void)written; // A failed stderr write cannot be reported safely from this handler.
    }
#else
    const auto fd = ::open(crash_dump_path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (fd != -1) {
        boost::stacktrace::safe_dump_to(fd);
        ::close(fd);
    } else {
        const auto written = ::write(STDERR_FILENO, failure, sizeof(failure) - 1);
        (void)written; // A failed stderr write cannot be reported safely from this handler.
    }
#endif
    std::abort();
}

void prepare_crash_dump()
{
    boost::system::error_code error;
    const auto                retained = boost::filesystem::exists("./backtrace.dump", error);
    if (error == boost::system::errc::no_such_file_or_directory)
        error.clear();
    if (retained || error) {
        crash_dump_path = boost::filesystem::unique_path("./backtrace-capture-%%%%-%%%%-%%%%-%%%%.dump").string();
        if (error)
            CASPAR_LOG(error) << L"Could not inspect crash capture path: " << u16(error.message());
        CASPAR_LOG(warning) << L"Preserving ./backtrace.dump; next crash capture will use " << u16(crash_dump_path);
    }
}

void report_previous_crash()
{
    const boost::filesystem::path dump("./backtrace.dump");
    boost::system::error_code     error;
    const auto                    exists = boost::filesystem::exists(dump, error);
    if (error == boost::system::errc::no_such_file_or_directory)
        return;
    if (error) {
        CASPAR_LOG(error) << L"Could not inspect previous crash dump: " << u16(error.message());
        return;
    }
    if (!exists)
        return;

    if (!boost::filesystem::is_regular_file(dump, error) || error) {
        CASPAR_LOG(error) << L"Previous crash dump is not a regular file or could not be inspected; retained at "
                             L"./backtrace.dump.";
        return;
    }

    boost::stacktrace::stacktrace trace;
#ifdef WIN32
    std::ifstream input(dump.string(), std::ios::binary);
    if (!input) {
        CASPAR_LOG(error) << L"Could not open previous crash dump; retained at ./backtrace.dump.";
        return;
    }
    trace = boost::stacktrace::stacktrace::from_dump(input);
    const auto read_failed = input.fail();
    input.close();
#else
    const auto fd = ::open(dump.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd == -1) {
        CASPAR_LOG(error) << L"Could not open previous crash dump; retained at ./backtrace.dump.";
        return;
    }
    struct stat info;
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
        ::close(fd);
        CASPAR_LOG(error) << L"Previous crash dump is not a regular file or could not be inspected; retained at "
                             L"./backtrace.dump.";
        return;
    }
    std::string contents;
    char        buffer[4096];
    bool        read_failed = false;
    for (;;) {
        const auto count = ::read(fd, buffer, sizeof(buffer));
        if (count > 0)
            contents.append(buffer, static_cast<std::size_t>(count));
        else if (count == 0)
            break;
        else if (errno != EINTR) {
            read_failed = true;
            break;
        }
    }
    ::close(fd);
    if (read_failed) {
        CASPAR_LOG(error) << L"Could not read previous crash dump; retained at ./backtrace.dump.";
        return;
    }
    std::istringstream input(contents, std::ios::in | std::ios::binary);
    trace = boost::stacktrace::stacktrace::from_dump(input);
    read_failed = input.fail();
#endif
    if (read_failed || trace.empty()) { // Keep incomplete evidence for manual inspection.
        CASPAR_LOG(error) << L"Previous crash dump is unreadable or empty; retained at ./backtrace.dump.";
        return;
    }

    // ASLR makes this process's symbol mappings unsuitable for the previous process's addresses.
    std::wostringstream report;
    report << L"Previous process crash (raw addresses; not symbolized across ASLR restarts):";
    for (std::size_t n = 0; n < trace.size(); ++n) {
        report << L"\n" << n << L"# 0x" << std::hex << reinterpret_cast<std::uintptr_t>(trace[n].address()) << std::dec;
    }
    report << L"\nUse a core dump and the matching executable/debug symbols for a reliable backtrace. "
              L"See BUILDING.md, Linux crash diagnostics.";
    CASPAR_LOG(error) << report.str();

    boost::filesystem::path archive;
    for (int attempt = 0; attempt < 8; ++attempt) {
        archive = boost::filesystem::unique_path("./backtrace-%%%%-%%%%-%%%%-%%%%.dump");
        boost::filesystem::create_hard_link(dump, archive, error); // Reserve the name without overwriting an archive.
        if (error != boost::system::errc::file_exists)
            break; // Retry only name collisions.
    }
    if (error) {
        CASPAR_LOG(error) << L"Could not archive previous crash dump; retained at ./backtrace.dump: "
                          << u16(error.message());
        return;
    }
    CASPAR_LOG(error) << L"Previous crash dump preserved at " << archive.wstring();
    boost::filesystem::remove(dump, error); // Release the original name only after preserving the dump.
    if (error)
        CASPAR_LOG(error) << L"Could not remove archived ./backtrace.dump; both paths retained: " << u16(error.message());
}

void terminate_handler()
{
    try {
        std::rethrow_exception(std::current_exception());
    } catch (...) {
        CASPAR_LOG_CURRENT_EXCEPTION();
    }
    std::abort(); // Capture the stack through the SIGABRT handler.
}

} // namespace caspar

int main(int argc, char** argv)
{
    using namespace caspar;

    // Before the interceptors, as the CEF subprocesses return from there without reaching the rest
    // of main and need the same scheduling guarantees as the browser process.
    setup_process_scheduling();

    if (intercept_command_line_args(argc, argv))
        return 0;

    int return_code = 0;
    setup_prerequisites();

    setup_global_locale();

    std::wcout << L"Type \"q\" to close application." << std::endl;

    // Set debug mode.
    auto debugging_environment = setup_debugging_environment();

    // Increase process priority.
    increase_process_priority();

    std::wstring config_file_name(L"casparcg.config");

    try {
        // Configure environment properties from configuration.
        if (argc >= 2)
            config_file_name = caspar::u16(argv[1]);

        log::add_cout_sink();
        env::configure(config_file_name);

        log::set_log_column_alignment(env::properties().get(L"configuration.log-align-columns", true));

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
        if (env::log_to_file()) {
            log::add_file_sink(env::log_folder() + L"caspar");
            std::wcout << L"Logging [" << log::get_log_level() << L"] or higher severity to " << env::log_folder()
                       << std::endl
                       << std::endl;
        } else {
            std::wcout << L"Logging [" << log::get_log_level() << L"] or higher severity to console" << std::endl
                       << std::endl;
        }

        // Logging is ready; archive old evidence before enabling handlers that could overwrite it.
        report_previous_crash();
        prepare_crash_dump();

        ::signal(SIGSEGV, signal_handler);
        ::signal(SIGABRT, signal_handler);
        ::signal(SIGILL, signal_handler);
        std::set_terminate(caspar::terminate_handler);

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
    } catch (caspar::ptree_exception& e) {
        auto info = boost::get_error_info<caspar::msg_info_t>(e);
        if (info) {
            CASPAR_LOG(fatal) << *info << ". Please check the configuration file (" << u8(config_file_name)
                              << ") for errors.";
        } else {
            CASPAR_LOG(fatal) << "Please check the configuration file (" << u8(config_file_name) << ") for errors.";
            CASPAR_LOG_CURRENT_EXCEPTION();
        }
        wait_for_keypress();
    } catch (boost::property_tree::file_parser_error& e) {
        CASPAR_LOG(fatal) << "At " << u8(config_file_name) << ":" << e.line() << ": " << e.message()
                          << ". Please check the configuration file (" << u8(config_file_name) << ") for errors.";
        wait_for_keypress();
    } catch (expected_user_error&) {
        CASPAR_LOG(fatal) << " Please check the configuration file (" << u8(config_file_name) << ") for errors.";
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

    boost::log::core::get()->flush();

    return return_code;
}
