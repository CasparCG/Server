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

#include "frame_producer.h"

#include "../fwd.h"
#include "../monitor/monitor.h"

#include <common/memory.h>

namespace caspar { namespace core {

class layer final
{
    layer(const layer&);
    layer& operator=(const layer&);

  public:
    explicit layer();
    layer(layer&& other);

    layer& operator=(layer&& other);

    void swap(layer& other);

    void load(spl::shared_ptr<frame_producer> producer, bool preview, bool auto_play = false);
    void play();
    void preview();
    void pause();
    void resume();
    void stop();

    draw_frame receive(const video_format_desc& format_desc, int nb_samples);
    draw_frame receive_background(const video_format_desc& format_desc, int nb_samples);

    core::monitor::state state() const;

    spl::shared_ptr<frame_producer> foreground() const;
    spl::shared_ptr<frame_producer> background() const;
    bool                            has_background() const;

  private:
    struct impl;
    spl::shared_ptr<impl> impl_;
};

}} // namespace caspar::core
