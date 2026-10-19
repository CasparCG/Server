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

#include <portaudio.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace caspar { namespace proaudio {

// Case/whitespace-insensitive name comparison helper: lowercases and strips non-printable/space chars.
std::string clean_name(const std::string& s);

// Parses a CHANNEL_MAP param like "1,3,4" into 1-based channel numbers. An empty string means
// "identity map", i.e. 1..default_channels.
std::vector<int> parse_channel_map(const std::string& str, int default_channels);

enum class device_direction
{
    input,
    output
};

// Finds a PortAudio device by (fuzzy, case/whitespace-insensitive) name and, optionally, host API
// name (e.g. "ASIO", "WASAPI", "ALSA", "JACK") to disambiguate identically-named devices exposed
// through more than one host API. Empty device_name + empty host_api_name means "the default
// input/output device". Throws invalid_operation (after logging all available devices) if nothing
// matches.
PaDeviceIndex
find_device(device_direction direction, const std::string& device_name, const std::string& host_api_name);

// PortAudio's Pa_Initialize/Pa_Terminate must be balanced process-wide. Refcounted so the first
// consumer/producer initializes it and the last one tears it down.
class pa_library
{
    pa_library();

    pa_library(const pa_library&)            = delete;
    pa_library& operator=(const pa_library&) = delete;

    inline static std::mutex             mutex_;
    inline static std::weak_ptr<pa_library> instance_;

  public:
    ~pa_library();

    static std::shared_ptr<pa_library> acquire();
};

}} // namespace caspar::proaudio
