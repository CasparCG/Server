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

#include "device_util.h"

#include <common/except.h>
#include <common/log.h>
#include <common/utf.h>

#include <boost/algorithm/string.hpp>

#include <cctype>
#include <sstream>

namespace caspar { namespace proaudio {

std::string clean_name(const std::string& s)
{
    std::string out;
    for (unsigned char c : s) {
        if (std::isgraph(c))
            out += static_cast<char>(std::tolower(c));
    }
    return out;
}

std::vector<int> parse_channel_map(const std::string& str, int default_channels)
{
    std::vector<int> map;

    if (str.empty()) {
        for (int i = 0; i < default_channels; ++i)
            map.push_back(i + 1);
        return map;
    }

    std::stringstream ss(str);
    std::string       item;
    while (std::getline(ss, item, ',')) {
        boost::trim(item);
        if (item.empty())
            continue;

        std::size_t pos   = 0;
        auto        value = std::stoi(item, &pos);
        if (pos != item.size())
            throw std::invalid_argument("CHANNEL_MAP entry is not a plain integer: '" + item + "'");

        map.push_back(value);
    }
    return map;
}

PaDeviceIndex
find_device(device_direction direction, const std::string& device_name, const std::string& host_api_name)
{
    const auto is_output = direction == device_direction::output;

    if (device_name.empty() && host_api_name.empty())
        return is_output ? Pa_GetDefaultOutputDevice() : Pa_GetDefaultInputDevice();

    auto target_name = clean_name(device_name);
    auto target_api   = clean_name(host_api_name);

    std::vector<std::string> available;
    auto                     count = Pa_GetDeviceCount();

    for (PaDeviceIndex i = 0; i < count; ++i) {
        auto info     = Pa_GetDeviceInfo(i);
        auto channels = info ? (is_output ? info->maxOutputChannels : info->maxInputChannels) : 0;
        if (!info || channels <= 0)
            continue;

        auto api_info = Pa_GetHostApiInfo(info->hostApi);
        auto api_name  = api_info ? api_info->name : "?";

        available.push_back(std::string(info->name) + " (" + api_name + ")");

        if (!target_api.empty() && clean_name(api_name).find(target_api) == std::string::npos)
            continue;

        if (target_name.empty() || clean_name(info->name).find(target_name) != std::string::npos)
            return i;
    }

    CASPAR_LOG(info) << "-------- PortAudio Devices -------";
    for (auto&& name : available)
        CASPAR_LOG(info) << u16(name);
    CASPAR_LOG(info) << "-------- PortAudio Devices -------";

    CASPAR_THROW_EXCEPTION(invalid_operation()
                           << msg_info("Invalid PortAudio device/host-api: '" + device_name + "' '" + host_api_name +
                                       "'"));
}

pa_library::pa_library()
{
    auto err = Pa_Initialize();
    if (err != paNoError)
        CASPAR_THROW_EXCEPTION(invalid_operation()
                               << msg_info(std::string("Failed to initialize PortAudio: ") + Pa_GetErrorText(err)));
}

pa_library::~pa_library() { Pa_Terminate(); }

std::shared_ptr<pa_library> pa_library::acquire()
{
    std::lock_guard guard{mutex_};

    auto shared = instance_.lock();
    if (!shared)
        instance_ = shared = std::shared_ptr<pa_library>{new pa_library()};

    return shared;
}

}} // namespace caspar::proaudio
