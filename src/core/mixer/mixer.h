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

#include <common/bit_depth.h>
#include <common/memory.h>

#include <core/fwd.h>
#include <core/monitor/monitor.h>

namespace caspar::diagnostics {
class graph;
}

namespace caspar { namespace core {

class mixer final
{
    mixer(const mixer&);
    mixer& operator=(const mixer&);

  public:
    explicit mixer(int                                         channel_index,
                   spl::shared_ptr<caspar::diagnostics::graph> graph,
                   spl::shared_ptr<image_mixer>                image_mixer);

    /// Buffers the supplied transform snapshot with its frame until rendering completes.
    const_frame operator()(std::vector<draw_frame>  frames,
                           const video_format_desc& format_desc,
                           int                      nb_samples,
                           monitor::state           transforms);

    void  set_master_volume(float volume);
    float get_master_volume();

    mutable_frame create_frame(const void* tag, const pixel_format_desc& desc);

    core::monitor::state state() const;

    /// Returns the last successfully rendered frame's transforms, or empty state before priming.
    core::monitor::state transforms() const;

    common::bit_depth depth() const;

  private:
    struct impl;
    spl::shared_ptr<impl> impl_;
};

}} // namespace caspar::core
