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

#include "../fwd.h"
#include "../monitor/monitor.h"

#include <common/forward.h>
#include <common/memory.h>
#include <common/tweener.h>

#include <core/frame/draw_frame.h>

#include <functional>
#include <future>
#include <map>
#include <tuple>
#include <vector>

FORWARD2(caspar, diagnostics, class graph);

namespace caspar { namespace core {

struct layer_frame
{
    draw_frame foreground;
    draw_frame background;
    bool       has_background;
};

class stage final
{
    stage(const stage&);
    stage& operator=(const stage&);

  public:
    using transform_func_t  = std::function<struct frame_transform(struct frame_transform)>;
    using transform_tuple_t = std::tuple<int, transform_func_t, unsigned int, tweener>;

    explicit stage(int channel_index, spl::shared_ptr<caspar::diagnostics::graph> graph);

    std::vector<draw_frame> operator()(const video_format_desc&                     format_desc,
                                       int                                          nb_samples,
                                       std::vector<int>&                            fetch_background,
                                       std::function<void(int, const layer_frame&)> routesCb);

    std::future<void> apply_transforms(const std::vector<transform_tuple_t>& transforms);
    std::future<void>
                                 apply_transform(int index, const transform_func_t& transform, unsigned int mix_duration, const tweener& tween);
    std::future<void>            clear_transforms(int index);
    std::future<void>            clear_transforms();
    std::future<frame_transform> get_current_transform(int index);
    std::future<void>
                              load(int index, const spl::shared_ptr<frame_producer>& producer, bool preview = false, bool auto_play = false);
    std::future<void>         preview(int index);
    std::future<void>         pause(int index);
    std::future<void>         resume(int index);
    std::future<void>         play(int index);
    std::future<void>         stop(int index);
    std::future<std::wstring> call(int index, const std::vector<std::wstring>& params);
    std::future<void>         clear(int index);
    std::future<void>         clear();
    std::future<void>         swap_layers(stage& other, bool swap_transforms);
    std::future<void>         swap_layer(int index, int other_index, bool swap_transforms);
    std::future<void>         swap_layer(int index, int other_index, stage& other, bool swap_transforms);

    core::monitor::state state() const;

    std::future<std::shared_ptr<frame_producer>> foreground(int index);
    std::future<std::shared_ptr<frame_producer>> background(int index);

  private:
    struct impl;
    spl::shared_ptr<impl> impl_;
};

}} // namespace caspar::core
