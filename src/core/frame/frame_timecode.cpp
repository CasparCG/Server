/*
 * Copyright (c) 2018 Norsk rikskringkasting AS
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
 * Author: Julian Waller, julian@superfly.tv
 */

#include "frame_timecode.h"

#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/format.hpp>
#include <common/future.h>

namespace caspar { namespace core {

uint32_t max_frames_for_fps(const uint8_t fps)
{
    static uint32_t num_seconds = 24 * 60 * 60;

    return num_seconds * fps;
}

uint32_t validate(uint32_t frames, uint8_t fps)
{
    const uint32_t max = max_frames_for_fps(fps);

    uint32_t val = frames;
    if (val >= max)
        val -= max;

    return val;
}

frame_timecode validate(const frame_timecode& timecode, const int delta)
{
    const int max_frames = timecode.max_frames();
    int       val        = timecode.total_frames() + delta;
    val %= max_frames;
    if (val < 0)
        val += max_frames;

    return frame_timecode(static_cast<uint32_t>(val), timecode.fps());
}

frame_timecode::frame_timecode()
    : frames_(0)
    , fps_(0)
{
}

frame_timecode::frame_timecode(uint32_t frames, uint8_t fps)
    : frames_(validate(frames, fps))
    , fps_(fps)
{
}

void frame_timecode::get_components(uint8_t& hours,
                                    uint8_t& minutes,
                                    uint8_t& seconds,
                                    uint8_t& frames,
                                    bool     smpte_frames) const
{
    if (fps_ == 0)
        return;

    uint32_t total = total_frames();

    frames = total % fps_;
    if (smpte_frames && fps_ > 30)
        frames /= 2;
    total /= fps_;

    seconds = total % 60;
    total /= 60;

    minutes = total % 60;
    total /= 60;

    hours = total % 24;
}

uint32_t frame_timecode::max_frames() const { return max_frames_for_fps(fps_); }

unsigned int frame_timecode::bcd() const
{
    uint8_t hours, minutes, seconds, frames;
    get_components(hours, minutes, seconds, frames, true);

    unsigned int res = 0;

    res += ((hours / 10) << 4) + (hours % 10);
    res <<= 8;
    res += ((minutes / 10) << 4) + (minutes % 10);
    res <<= 8;
    res += ((seconds / 10) << 4) + (seconds % 10);
    res <<= 8;
    res += ((frames / 10) << 4) + (frames % 10);

    return res;
}

int64_t frame_timecode::pts() const
{
    if (fps_ == 0)
        return 0;

    int64_t res = total_frames();
    res *= 1000;
    res /= fps_;

    return res;
}

const std::wstring frame_timecode::string(bool smpte_frames) const
{
    uint8_t hours, minutes, seconds, frames;
    get_components(hours, minutes, seconds, frames, smpte_frames);
    return (boost::wformat(L"%02i:%02i:%02i:%02i") % hours % minutes % seconds % frames).str();
}

const frame_timecode& frame_timecode::empty()
{
    static const frame_timecode def = {0, 0};

    return def;
}

bool frame_timecode::parse_string(const std::wstring& str, uint8_t fps, bool smpte_frames, frame_timecode& res)
{
    if (str.length() != 11)
        return false;

    std::vector<std::wstring> strs;
    boost::split(strs, str, boost::is_any_of(":.;,"));

    if (strs.size() != 4)
        return false;

    try {
        const uint8_t hours   = static_cast<uint8_t>(std::stoi(strs[0]));
        const uint8_t minutes = static_cast<uint8_t>(std::stoi(strs[1]));
        const uint8_t seconds = static_cast<uint8_t>(std::stoi(strs[2]));
        uint8_t       frames  = static_cast<uint8_t>(std::stoi(strs[3]));

        return create(hours, minutes, seconds, frames, fps, smpte_frames, res);
    } catch (...) {
        return false;
    }
}

bool frame_timecode::create(uint8_t         hours,
                            uint8_t         minutes,
                            uint8_t         seconds,
                            uint8_t         frames,
                            uint8_t         fps,
                            bool            smpte_frames,
                            frame_timecode& res)
{
    if (smpte_frames && fps > 30)
        frames *= 2;

    if (hours > 23 || minutes > 59 || seconds > 59 || frames > fps)
        return false;

    int total_frames = 0;
    total_frames += hours;
    total_frames *= 60;

    total_frames += minutes;
    total_frames *= 60;

    total_frames += seconds;
    total_frames *= fps;

    total_frames += frames;

    res = frame_timecode(total_frames, fps);
    return true;
}

bool frame_timecode::operator<(const frame_timecode& other) const { return pts() < other.pts(); }
bool frame_timecode::operator>(const frame_timecode& other) const { return pts() > other.pts(); }
bool frame_timecode::operator<=(const frame_timecode& other) const { return pts() <= other.pts(); }
bool frame_timecode::operator>=(const frame_timecode& other) const { return pts() >= other.pts(); }
bool frame_timecode::operator==(const frame_timecode& other) const
{
    return pts() == other.pts() && fps_ == other.fps_;
}
bool frame_timecode::operator!=(const frame_timecode& other) const
{
    return pts() != other.pts() || fps_ != other.fps_;
}

frame_timecode frame_timecode::operator+=(int delta) { return *this = validate(*this, delta); }
frame_timecode frame_timecode::operator-=(int delta) { return *this = validate(*this, -delta); }
frame_timecode frame_timecode::operator+(int delta) const { return validate(*this, delta); }
frame_timecode frame_timecode::operator-(int delta) const { return validate(*this, -delta); }

frame_timecode frame_timecode::operator-(const frame_timecode& other) const
{
    return validate(*this, -static_cast<int>(other.total_frames()));
}
frame_timecode frame_timecode::operator+(const frame_timecode& other) const
{
    return validate(*this, other.total_frames());
}

frame_timecode frame_timecode::with_fps(uint8_t new_fps) const
{
    if (new_fps == fps_)
        return *this;

    const int64_t frames = pts() * new_fps / 1000;
    return frame_timecode(static_cast<uint32_t>(frames), new_fps);
}

}} // namespace caspar::core
