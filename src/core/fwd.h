/*
 * Copyright 2013 Sveriges Television AB http://casparcg.com/
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

#include <common/forward.h>

FORWARD2(caspar, accelerator, class accelerator);
FORWARD3(caspar, accelerator, ogl, class device);

FORWARD2(caspar, core, class stage);
FORWARD2(caspar, core, class mixer);
FORWARD2(caspar, core, class output);
FORWARD2(caspar, core, class image_mixer);
FORWARD2(caspar, core, struct video_format_desc);
FORWARD2(caspar, core, class frame_factory);
FORWARD2(caspar, core, class frame_producer);
FORWARD2(caspar, core, class frame_consumer);
FORWARD2(caspar, core, class draw_frame);
FORWARD2(caspar, core, class mutable_frame);
FORWARD2(caspar, core, class const_frame);
FORWARD2(caspar, core, struct frame_timecode);
FORWARD2(caspar, core, class timecode_provider);
FORWARD2(caspar, core, class video_channel);
FORWARD2(caspar, core, struct pixel_format_desc);
FORWARD2(caspar, core, class cg_producer_registry);
FORWARD2(caspar, core, struct frame_transform);
FORWARD2(caspar, core, struct write_frame_consumer);
FORWARD2(caspar, core, struct frame_producer_dependencies);
FORWARD2(caspar, core, struct module_dependencies);
FORWARD2(caspar, core, class frame_producer_registry);
