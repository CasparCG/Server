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

#pragma once

#include <cstdint>

namespace caspar { namespace core {

struct frame_timecode
{
    frame_timecode();
    frame_timecode(uint32_t frames, uint8_t fps);

    void get_components(uint8_t& hours, uint8_t& minutes, uint8_t& seconds, uint8_t& frames, bool smpte_frames) const;
    uint8_t  fps() const { return fps_; }
    uint32_t total_frames() const { return frames_; }
    uint32_t max_frames() const;

    unsigned int       bcd() const;
    int64_t            pts() const;
    const std::wstring string(bool smpte_frames) const;

    static const frame_timecode& empty();
    static bool parse_string(const std::wstring& str, uint8_t fps, bool smpte_frames, frame_timecode& res);
    static bool create(uint8_t         hours,
                       uint8_t         minutes,
                       uint8_t         seconds,
                       uint8_t         frames,
                       uint8_t         fps,
                       bool            smpte_frames,
                       frame_timecode& res);

    bool operator<(const frame_timecode& other) const;
    bool operator>(const frame_timecode& other) const;
    bool operator<=(const frame_timecode& other) const;
    bool operator>=(const frame_timecode& other) const;

    bool operator==(const frame_timecode& other) const;
    bool operator!=(const frame_timecode& other) const;

    frame_timecode operator+=(int frames);
    frame_timecode operator-=(int frames);
    frame_timecode operator+(int frames) const;
    frame_timecode operator-(int frames) const;

    frame_timecode operator-(const frame_timecode& other) const;
    frame_timecode operator+(const frame_timecode& other) const;

    frame_timecode with_fps(uint8_t new_fps) const;

    bool is_valid() const { return fps_ != 0; }

  private:
    uint32_t frames_;
    uint8_t  fps_;
};

}} // namespace caspar::core
