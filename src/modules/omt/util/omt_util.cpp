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
#include "omt_util.h"

#include <common/env.h>
#include <common/except.h>
#include <common/log.h>
#include <common/utf.h>

#include <boost/filesystem.hpp>
#include <boost/locale.hpp>

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace caspar { namespace omt {

namespace {

// --- Loading the library ---

#ifdef _WIN32
const wchar_t* const library_filename = L"libomt.dll";

// Sets error to the reason when the library cannot be loaded, for example because it is missing.
void* open_module(const boost::filesystem::path& path, std::wstring& error)
{
    void* const module = LoadLibraryW(path.c_str());
    if (module == nullptr) {
        const DWORD code   = GetLastError();
        wchar_t*    buffer = nullptr;
        const DWORD length =
            FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                           nullptr,
                           code,
                           0,
                           reinterpret_cast<LPWSTR>(&buffer),
                           0,
                           nullptr);

        std::wstring reason = length > 0 ? std::wstring(buffer, length) : L"error " + std::to_wstring(code);
        LocalFree(buffer);
        while (reason.empty() == false && (reason.back() == L'\r' || reason.back() == L'\n' || reason.back() == L' ')) {
            reason.pop_back();
        }
        error = path.wstring() + L": " + reason;
    }
    return module;
}

void* find_symbol(void* module, const char* name)
{
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(module), name));
}

#else
const char* const library_filename = "libomt.so";

// Sets error to the reason when the library cannot be loaded, for example because one of the libraries
// that it needs is missing.
void* open_module(const boost::filesystem::path& path, std::wstring& error)
{
    void* const module = dlopen(path.c_str(), RTLD_LOCAL | RTLD_LAZY);
    if (module == nullptr) {
        const char* const reason = dlerror();
        error                    = u16(std::string(reason != nullptr ? reason : "unknown error"));
    }
    return module;
}

void* find_symbol(void* module, const char* name) { return dlsym(module, name); }

#endif

// load_errors says why the system could not load the candidates. A libomt that is installed but cannot be
// loaded, for example because a library that it needs is missing, would otherwise look like a missing one.
[[noreturn]] void throw_library_not_found(const std::wstring& load_errors)
{
    std::wstring message = L"libomt was not found. It is built together with CasparCG, unless CMake was "
                           L"configured with -DOMT_BUILD_FROM_SOURCE=OFF. Otherwise libomt "
                           L"(https://github.com/openmediatransport/libomt) has to be installed where the system "
                           L"can find it.";
    if (load_errors.empty() == false) {
        message += L" The system could not load it: " + load_errors;
    }
    CASPAR_THROW_EXCEPTION(not_supported() << msg_info(message));
}

[[noreturn]] void throw_library_incompatible()
{
    CASPAR_THROW_EXCEPTION(not_supported() << msg_info(L"libomt is missing one or more functions that CasparCG needs. "
                                                       L"The installed version may not work with this CasparCG."));
}

template <typename Function>
bool find_function(void* module, const char* name, Function& function)
{
    function = reinterpret_cast<Function>(find_symbol(module, name));
    return function != nullptr;
}

// Returns false if the library lacks a function that CasparCG needs.
bool find_functions(void* module, omt_lib& lib)
{
    bool all_functions_found = true;
    all_functions_found &= find_function(module, "omt_discovery_getaddresses", lib.discovery_getaddresses);
    all_functions_found &= find_function(module, "omt_receive_create", lib.receive_create);
    all_functions_found &= find_function(module, "omt_receive_destroy", lib.receive_destroy);
    all_functions_found &= find_function(module, "omt_receive", lib.receive);
    all_functions_found &= find_function(module, "omt_send_create", lib.send_create);
    all_functions_found &= find_function(module, "omt_send_destroy", lib.send_destroy);
    all_functions_found &= find_function(module, "omt_send", lib.send);
    all_functions_found &= find_function(module, "omt_send_getaddress", lib.send_getaddress);
    all_functions_found &= find_function(module, "omt_send_connections", lib.send_connections);

    // Not every libomt has it, so it is not required.
    find_function(module, "omt_shutdown", lib.shutdown);

    return all_functions_found;
}

// Looks for libomt on the system library path first, then in the folder CasparCG was started from, and
// takes the first one that has all the functions that CasparCG needs. A libomt on the system that is too
// old must not hide a good one next to the server.
std::unique_ptr<omt_lib> open_library()
{
    bool         found_incompatible_library = false;
    std::wstring load_errors;
    for (int attempt = 0; attempt < 2; ++attempt) {
        // The folder CasparCG was started from is only looked up if the system has no usable libomt.
        const boost::filesystem::path candidate =
            attempt == 0 ? boost::filesystem::path(library_filename)
                         : boost::filesystem::path(env::initial_folder()) / library_filename;

        std::wstring error;
        void* const  module = open_module(candidate, error);
        if (module == nullptr) {
            CASPAR_LOG(debug) << L"Could not load libomt: " << error;
            if (load_errors.empty() == false) {
                load_errors += L"; ";
            }
            load_errors += error;
            continue;
        }

        // No library is ever unloaded, not even one that is skipped: libomt runs threads of its own (discovery,
        // logging), and unloading it while they run crashes the process. The system unloads them when
        // CasparCG exits.
        auto loaded = std::make_unique<omt_lib>();
        if (find_functions(module, *loaded) == false) {
            CASPAR_LOG(warning) << L"Skipped " << candidate.wstring()
                                << L", it is missing functions that CasparCG needs.";
            found_incompatible_library = true;
            continue;
        }

        CASPAR_LOG(info) << L"Loaded " << candidate.wstring();
        return loaded;
    }

    if (found_incompatible_library == true) {
        throw_library_incompatible();
    }
    throw_library_not_found(load_errors);
}

// --- Names ---

bool is_ascii(wchar_t character) { return static_cast<unsigned int>(character) <= 127; }

bool is_ascii(const std::wstring& text)
{
    for (const wchar_t character : text) {
        if (is_ascii(character) == false) {
            return false;
        }
    }
    return true;
}

bool is_combining_mark(wchar_t character) { return character >= 0x0300 && character <= 0x036F; }

// Splits accented letters into the letter and its combining marks, then drops the marks.
std::wstring remove_diacritics(const std::wstring& text)
{
    // The process-wide locale has no "convert" facet, which normalize() needs.
    static const boost::locale::generator generator;
    static const std::locale              normalization_locale = generator("en_US.UTF-8");

    const std::wstring decomposed = boost::locale::normalize(text, boost::locale::norm_nfd, normalization_locale);

    std::wstring result;
    result.reserve(decomposed.size());
    for (const wchar_t character : decomposed) {
        if (is_combining_mark(character) == false) {
            result.push_back(character);
        }
    }
    return result;
}

} // namespace

omt_lib* load_library()
{
    static omt_lib* const lib = [] {
        auto loaded = open_library();

        if (loaded->shutdown != nullptr) {
            // By the time statics are destroyed, the producers and consumers are gone.
            static const struct shutdown_at_exit
            {
                decltype(&::omt_shutdown) shutdown;
                ~shutdown_at_exit() { shutdown(); }
            } stop_threads_at_exit{loaded->shutdown};
        }

        return loaded.release();
    }();

    return lib;
}

std::unique_lock<std::mutex> serialize_create_call()
{
    static std::mutex                                           mutex;
    static std::optional<std::chrono::steady_clock::time_point> last_call_time;

    constexpr auto minimum_gap = std::chrono::milliseconds(300);

    std::unique_lock<std::mutex> lock(mutex);

    if (last_call_time.has_value() == true) {
        const auto time_since_last_call = std::chrono::steady_clock::now() - last_call_time.value();
        if (time_since_last_call < minimum_gap) {
            std::this_thread::sleep_for(minimum_gap - time_since_last_call);
        }
    }
    last_call_time = std::chrono::steady_clock::now();

    return lock;
}

std::wstring make_ascii_safe_name(const std::wstring& name)
{
    if (is_ascii(name) == true) {
        return name;
    }

    std::wstring result;
    try {
        result = remove_diacritics(name);
    } catch (...) {
        // Without normalization everything non-ASCII is replaced below.
        result = name;
    }

    for (wchar_t& character : result) {
        if (is_ascii(character) == false) {
            character = L'_';
        }
    }
    return result;
}

std::vector<std::string> get_current_sources()
{
    // libomt keeps the array it returns in static storage and frees it on the next call, so the calls and the
    // copies out of it are made one at a time. Every AMCP connection runs its commands on its own thread.
    static std::mutex           discovery_mutex;
    std::lock_guard<std::mutex> lock(discovery_mutex);

    int          address_count = 0;
    char** const addresses     = load_library()->discovery_getaddresses(&address_count);

    std::vector<std::string> sources;
    if (addresses == nullptr) {
        return sources;
    }

    for (int index = 0; index < address_count; ++index) {
        if (addresses[index] != nullptr) {
            sources.emplace_back(addresses[index]);
        }
    }
    return sources;
}

std::wstring list_command(protocol::amcp::command_context& /*ctx*/)
{
    const auto sources = get_current_sources();

    std::wstringstream reply;
    reply << L"200 OMT LIST OK\r\n";
    for (size_t index = 0; index < sources.size(); ++index) {
        reply << (index + 1) << L" \"" << u16(sources[index]) << L"\"\r\n";
    }
    reply << L"\r\n";
    return reply.str();
}

}} // namespace caspar::omt
