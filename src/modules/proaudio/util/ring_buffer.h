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

#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

namespace caspar { namespace proaudio {

// Single-producer/single-consumer lock-free ring buffer of interleaved audio frames.
//
// push() and pop() never block or take a lock against each other, so each may safely be called
// from a realtime PortAudio callback that must never block, allocate, or touch a mutex - which
// side that is depends on the direction of data flow: the consumer pushes from its executor
// thread and pops from its realtime playback callback, while the producer is the other way
// around (pushes from its realtime capture callback, pops from the mixer's executor thread).
// Either way, push() must only ever be called from one fixed thread and pop() from the other.
//
// One "frame" here is `channels` interleaved int16_t samples.
class frame_ring_buffer
{
  public:
    explicit frame_ring_buffer(std::size_t capacity_frames, std::size_t channels)
        : channels_(channels)
        , capacity_(next_pow2(capacity_frames + 1))
        , mask_(capacity_ - 1)
        , buffer_(capacity_ * channels)
    {
    }

    std::size_t channels() const { return channels_; }

    // Called only from whichever side owns writing (see class comment). Never blocks/allocates.
    // Returns the number of whole frames actually written (less than requested if full).
    std::size_t push(const int16_t* frames, std::size_t frame_count)
    {
        auto write = write_.load(std::memory_order_relaxed);
        auto read  = read_.load(std::memory_order_acquire);

        auto free_frames = capacity_ - (write - read);
        auto to_write     = std::min(frame_count, free_frames);

        for (std::size_t i = 0; i < to_write; ++i) {
            auto slot = (write + i) & mask_;
            std::memcpy(&buffer_[slot * channels_], &frames[i * channels_], channels_ * sizeof(int16_t));
        }

        write_.store(write + to_write, std::memory_order_release);
        return to_write;
    }

    // Called only from whichever side owns reading (see class comment). Never blocks/allocates.
    // Missing frames (buffer underrun) are zero-filled; returns the number of frames that were
    // real audio (as opposed to underrun silence), so the caller can flag a dropout.
    std::size_t pop(int16_t* out_frames, std::size_t frame_count)
    {
        auto read  = read_.load(std::memory_order_relaxed);
        auto write = write_.load(std::memory_order_acquire);

        auto available = write - read;
        auto to_read    = std::min(frame_count, available);

        for (std::size_t i = 0; i < to_read; ++i) {
            auto slot = (read + i) & mask_;
            std::memcpy(&out_frames[i * channels_], &buffer_[slot * channels_], channels_ * sizeof(int16_t));
        }
        if (to_read < frame_count) {
            std::memset(&out_frames[to_read * channels_], 0, (frame_count - to_read) * channels_ * sizeof(int16_t));
        }

        read_.store(read + to_read, std::memory_order_release);
        return to_read;
    }

  private:
    static std::size_t next_pow2(std::size_t v)
    {
        std::size_t p = 1;
        while (p < v)
            p <<= 1;
        return p;
    }

    const std::size_t channels_;
    const std::size_t capacity_;
    const std::size_t mask_;

    std::vector<int16_t> buffer_;

    alignas(64) std::atomic<std::size_t> write_{0};
    alignas(64) std::atomic<std::size_t> read_{0};
};

}} // namespace caspar::proaudio
