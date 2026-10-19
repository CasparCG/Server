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
#include "frame_transform.h"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/range/algorithm/equal.hpp>

#include <cmath>
#include <utility>

namespace caspar { namespace core {

double do_tween(double time, double source, double dest, double duration, const tweener& tween)
{
    return tween(time, source, dest - source, duration);
}

template <typename Rect>
void do_tween_rectangle(const Rect&    source,
                        const Rect&    dest,
                        Rect&          out,
                        double         time,
                        double         duration,
                        const tweener& tweener)
{
    out.ul[0] = do_tween(time, source.ul[0], dest.ul[0], duration, tweener);
    out.ul[1] = do_tween(time, source.ul[1], dest.ul[1], duration, tweener);
    out.lr[0] = do_tween(time, source.lr[0], dest.lr[0], duration, tweener);
    out.lr[1] = do_tween(time, source.lr[1], dest.lr[1], duration, tweener);
}

void do_tween_corners(const corners& source,
                      const corners& dest,
                      corners&       out,
                      double         time,
                      double         duration,
                      const tweener& tweener)
{
    do_tween_rectangle(source, dest, out, time, duration, tweener);

    out.ur[0] = do_tween(time, source.ur[0], dest.ur[0], duration, tweener);
    out.ur[1] = do_tween(time, source.ur[1], dest.ur[1], duration, tweener);
    out.ll[0] = do_tween(time, source.ll[0], dest.ll[0], duration, tweener);
    out.ll[1] = do_tween(time, source.ll[1], dest.ll[1], duration, tweener);
}

image_transform image_transform::tween(double                 time,
                                       const image_transform& source,
                                       const image_transform& dest,
                                       double                 duration,
                                       const tweener&         tween)
{
    image_transform result;

    result.brightness          = do_tween(time, source.brightness, dest.brightness, duration, tween);
    result.contrast            = do_tween(time, source.contrast, dest.contrast, duration, tween);
    result.saturation          = do_tween(time, source.saturation, dest.saturation, duration, tween);
    result.opacity             = do_tween(time, source.opacity, dest.opacity, duration, tween);
    result.anchor[0]           = do_tween(time, source.anchor[0], dest.anchor[0], duration, tween);
    result.anchor[1]           = do_tween(time, source.anchor[1], dest.anchor[1], duration, tween);
    result.fill_translation[0] = do_tween(time, source.fill_translation[0], dest.fill_translation[0], duration, tween);
    result.fill_translation[1] = do_tween(time, source.fill_translation[1], dest.fill_translation[1], duration, tween);
    result.fill_scale[0]       = do_tween(time, source.fill_scale[0], dest.fill_scale[0], duration, tween);
    result.fill_scale[1]       = do_tween(time, source.fill_scale[1], dest.fill_scale[1], duration, tween);
    result.clip_translation[0] = do_tween(time, source.clip_translation[0], dest.clip_translation[0], duration, tween);
    result.clip_translation[1] = do_tween(time, source.clip_translation[1], dest.clip_translation[1], duration, tween);
    result.clip_scale[0]       = do_tween(time, source.clip_scale[0], dest.clip_scale[0], duration, tween);
    result.clip_scale[1]       = do_tween(time, source.clip_scale[1], dest.clip_scale[1], duration, tween);
    result.angle               = do_tween(time, source.angle, dest.angle, duration, tween);
    result.levels.max_input    = do_tween(time, source.levels.max_input, dest.levels.max_input, duration, tween);
    result.levels.min_input    = do_tween(time, source.levels.min_input, dest.levels.min_input, duration, tween);
    result.levels.max_output   = do_tween(time, source.levels.max_output, dest.levels.max_output, duration, tween);
    result.levels.min_output   = do_tween(time, source.levels.min_output, dest.levels.min_output, duration, tween);
    result.levels.gamma        = do_tween(time, source.levels.gamma, dest.levels.gamma, duration, tween);
    result.chroma.target_hue   = do_tween(time, source.chroma.target_hue, dest.chroma.target_hue, duration, tween);
    result.chroma.hue_width    = do_tween(time, source.chroma.hue_width, dest.chroma.hue_width, duration, tween);
    result.chroma.min_saturation =
        do_tween(time, source.chroma.min_saturation, dest.chroma.min_saturation, duration, tween);
    result.chroma.min_brightness =
        do_tween(time, source.chroma.min_brightness, dest.chroma.min_brightness, duration, tween);
    result.chroma.softness = do_tween(time, source.chroma.softness, dest.chroma.softness, duration, tween);
    result.chroma.spill_suppress =
        do_tween(time, source.chroma.spill_suppress, dest.chroma.spill_suppress, duration, tween);
    result.chroma.spill_suppress_saturation =
        do_tween(time, source.chroma.spill_suppress_saturation, dest.chroma.spill_suppress_saturation, duration, tween);
    result.chroma.enable    = dest.chroma.enable;
    result.chroma.show_mask = dest.chroma.show_mask;
    result.is_key           = source.is_key || dest.is_key;
    result.invert           = source.invert || dest.invert;
    result.is_mix           = source.is_mix || dest.is_mix;
    result.blend_mode       = std::max(source.blend_mode, dest.blend_mode);
    result.layer_depth      = dest.layer_depth;

    result.gamut_compress = dest.gamut_compress;
    result.gc_cyan    = do_tween(time, source.gc_cyan, dest.gc_cyan, duration, tween);
    result.gc_magenta = do_tween(time, source.gc_magenta, dest.gc_magenta, duration, tween);
    result.gc_yellow  = do_tween(time, source.gc_yellow, dest.gc_yellow, duration, tween);

    result.qualifier_enable = dest.qualifier_enable;
    result.qual_target_hue  = do_tween(time, source.qual_target_hue, dest.qual_target_hue, duration, tween);
    result.qual_hue_width   = do_tween(time, source.qual_hue_width, dest.qual_hue_width, duration, tween);
    result.qual_min_sat     = do_tween(time, source.qual_min_sat, dest.qual_min_sat, duration, tween);
    result.qual_max_sat     = do_tween(time, source.qual_max_sat, dest.qual_max_sat, duration, tween);
    result.qual_min_lum     = do_tween(time, source.qual_min_lum, dest.qual_min_lum, duration, tween);
    result.qual_max_lum     = do_tween(time, source.qual_max_lum, dest.qual_max_lum, duration, tween);
    result.qual_softness    = do_tween(time, source.qual_softness, dest.qual_softness, duration, tween);
    result.qual_exposure    = do_tween(time, source.qual_exposure, dest.qual_exposure, duration, tween);
    result.qual_sat_offset  = do_tween(time, source.qual_sat_offset, dest.qual_sat_offset, duration, tween);
    result.qual_hue_offset  = do_tween(time, source.qual_hue_offset, dest.qual_hue_offset, duration, tween);

    auto rl = [&](double s, double d) { return do_tween(time, s, d, duration, tween); };
    result.per_channel_levels.enable       = dest.per_channel_levels.enable;
    result.per_channel_levels.r.min_input  = rl(source.per_channel_levels.r.min_input, dest.per_channel_levels.r.min_input);
    result.per_channel_levels.r.max_input  = rl(source.per_channel_levels.r.max_input, dest.per_channel_levels.r.max_input);
    result.per_channel_levels.r.gamma      = rl(source.per_channel_levels.r.gamma, dest.per_channel_levels.r.gamma);
    result.per_channel_levels.r.min_output = rl(source.per_channel_levels.r.min_output, dest.per_channel_levels.r.min_output);
    result.per_channel_levels.r.max_output = rl(source.per_channel_levels.r.max_output, dest.per_channel_levels.r.max_output);
    result.per_channel_levels.g.min_input  = rl(source.per_channel_levels.g.min_input, dest.per_channel_levels.g.min_input);
    result.per_channel_levels.g.max_input  = rl(source.per_channel_levels.g.max_input, dest.per_channel_levels.g.max_input);
    result.per_channel_levels.g.gamma      = rl(source.per_channel_levels.g.gamma, dest.per_channel_levels.g.gamma);
    result.per_channel_levels.g.min_output = rl(source.per_channel_levels.g.min_output, dest.per_channel_levels.g.min_output);
    result.per_channel_levels.g.max_output = rl(source.per_channel_levels.g.max_output, dest.per_channel_levels.g.max_output);
    result.per_channel_levels.b.min_input  = rl(source.per_channel_levels.b.min_input, dest.per_channel_levels.b.min_input);
    result.per_channel_levels.b.max_input  = rl(source.per_channel_levels.b.max_input, dest.per_channel_levels.b.max_input);
    result.per_channel_levels.b.gamma      = rl(source.per_channel_levels.b.gamma, dest.per_channel_levels.b.gamma);
    result.per_channel_levels.b.min_output = rl(source.per_channel_levels.b.min_output, dest.per_channel_levels.b.min_output);
    result.per_channel_levels.b.max_output = rl(source.per_channel_levels.b.max_output, dest.per_channel_levels.b.max_output);

    result.temperature = do_tween(time, source.temperature, dest.temperature, duration, tween);
    result.tint        = do_tween(time, source.tint, dest.tint, duration, tween);
    for (int i = 0; i < 3; ++i) {
        result.lift[i]    = do_tween(time, source.lift[i], dest.lift[i], duration, tween);
        result.midtone[i] = do_tween(time, source.midtone[i], dest.midtone[i], duration, tween);
        result.gain[i]    = do_tween(time, source.gain[i], dest.gain[i], duration, tween);
    }
    result.hue_shift = do_tween(time, source.hue_shift, dest.hue_shift, duration, tween);
    result.shadows    = do_tween(time, source.shadows, dest.shadows, duration, tween);
    result.highlights = do_tween(time, source.highlights, dest.highlights, duration, tween);
    for (int i = 0; i < 3; ++i) {
        result.split_shadow_color[i] =
            do_tween(time, source.split_shadow_color[i], dest.split_shadow_color[i], duration, tween);
        result.split_highlight_color[i] =
            do_tween(time, source.split_highlight_color[i], dest.split_highlight_color[i], duration, tween);
    }
    result.split_balance = do_tween(time, source.split_balance, dest.split_balance, duration, tween);
    for (int i = 0; i < 3; ++i) {
        result.cdl_slope[i]  = do_tween(time, source.cdl_slope[i], dest.cdl_slope[i], duration, tween);
        result.cdl_offset[i] = do_tween(time, source.cdl_offset[i], dest.cdl_offset[i], duration, tween);
        result.cdl_power[i]  = do_tween(time, source.cdl_power[i], dest.cdl_power[i], duration, tween);
    }
    result.cdl_saturation = do_tween(time, source.cdl_saturation, dest.cdl_saturation, duration, tween);

    do_tween_rectangle(source.crop, dest.crop, result.crop, time, duration, tween);
    do_tween_corners(source.perspective, dest.perspective, result.perspective, time, duration, tween);

    return result;
}

bool eq(double lhs, double rhs) { return std::abs(lhs - rhs) < 5e-8; }

bool operator==(const corners& lhs, const corners& rhs)
{
    return boost::range::equal(lhs.ul, rhs.ul, eq) && boost::range::equal(lhs.ur, rhs.ur, eq) &&
           boost::range::equal(lhs.lr, rhs.lr, eq) && boost::range::equal(lhs.ll, rhs.ll, eq);
}

bool operator==(const rectangle& lhs, const rectangle& rhs)
{
    return boost::range::equal(lhs.ul, rhs.ul, eq) && boost::range::equal(lhs.lr, rhs.lr, eq);
}

bool operator==(const image_transform& lhs, const image_transform& rhs)
{
    return eq(lhs.opacity, rhs.opacity) && eq(lhs.contrast, rhs.contrast) && eq(lhs.brightness, rhs.brightness) &&
               eq(lhs.saturation, rhs.saturation) && boost::range::equal(lhs.anchor, rhs.anchor, eq) &&
               boost::range::equal(lhs.fill_translation, rhs.fill_translation, eq) &&
               boost::range::equal(lhs.fill_scale, rhs.fill_scale, eq) &&
               boost::range::equal(lhs.clip_translation, rhs.clip_translation, eq) &&
               boost::range::equal(lhs.clip_scale, rhs.clip_scale, eq) && eq(lhs.angle, rhs.angle) &&
               lhs.is_key == rhs.is_key && lhs.invert == rhs.invert && lhs.is_mix == rhs.is_mix &&
               lhs.blend_mode == rhs.blend_mode && lhs.layer_depth == rhs.layer_depth &&
               lhs.chroma.enable == rhs.chroma.enable && lhs.chroma.show_mask == rhs.chroma.show_mask &&
               eq(lhs.chroma.target_hue, rhs.chroma.target_hue) && eq(lhs.chroma.hue_width, rhs.chroma.hue_width) &&
               eq(lhs.chroma.min_saturation, rhs.chroma.min_saturation) &&
               eq(lhs.chroma.min_brightness, rhs.chroma.min_brightness) &&
               eq(lhs.chroma.softness, rhs.chroma.softness) &&
               eq(lhs.chroma.spill_suppress, rhs.chroma.spill_suppress) &&
               eq(lhs.chroma.spill_suppress_saturation, rhs.chroma.spill_suppress_saturation) && lhs.crop == rhs.crop &&
               lhs.perspective == rhs.perspective && eq(lhs.temperature, rhs.temperature) &&
               eq(lhs.tint, rhs.tint) && boost::range::equal(lhs.lift, rhs.lift, eq) &&
               boost::range::equal(lhs.midtone, rhs.midtone, eq) &&
               boost::range::equal(lhs.gain, rhs.gain, eq) && eq(lhs.hue_shift, rhs.hue_shift) &&
               eq(lhs.shadows, rhs.shadows) && eq(lhs.highlights, rhs.highlights) &&
               boost::range::equal(lhs.split_shadow_color, rhs.split_shadow_color, eq) &&
               boost::range::equal(lhs.split_highlight_color, rhs.split_highlight_color, eq) &&
               eq(lhs.split_balance, rhs.split_balance) &&
               boost::range::equal(lhs.cdl_slope, rhs.cdl_slope, eq) &&
               boost::range::equal(lhs.cdl_offset, rhs.cdl_offset, eq) &&
               boost::range::equal(lhs.cdl_power, rhs.cdl_power, eq) &&
               eq(lhs.cdl_saturation, rhs.cdl_saturation) &&
               lhs.gamut_compress == rhs.gamut_compress &&
               eq(lhs.gc_cyan, rhs.gc_cyan) &&
               eq(lhs.gc_magenta, rhs.gc_magenta) &&
               eq(lhs.gc_yellow, rhs.gc_yellow) &&
               lhs.qualifier_enable == rhs.qualifier_enable &&
               eq(lhs.qual_target_hue, rhs.qual_target_hue) &&
               eq(lhs.qual_hue_width, rhs.qual_hue_width) &&
               eq(lhs.qual_min_sat, rhs.qual_min_sat) &&
               eq(lhs.qual_max_sat, rhs.qual_max_sat) &&
               eq(lhs.qual_min_lum, rhs.qual_min_lum) &&
               eq(lhs.qual_max_lum, rhs.qual_max_lum) &&
               eq(lhs.qual_softness, rhs.qual_softness) &&
               eq(lhs.qual_exposure, rhs.qual_exposure) &&
               eq(lhs.qual_sat_offset, rhs.qual_sat_offset) &&
               eq(lhs.qual_hue_offset, rhs.qual_hue_offset) &&
               lhs.per_channel_levels.enable == rhs.per_channel_levels.enable &&
               eq(lhs.per_channel_levels.r.min_input, rhs.per_channel_levels.r.min_input) &&
               eq(lhs.per_channel_levels.r.max_input, rhs.per_channel_levels.r.max_input) &&
               eq(lhs.per_channel_levels.r.gamma, rhs.per_channel_levels.r.gamma) &&
               eq(lhs.per_channel_levels.r.min_output, rhs.per_channel_levels.r.min_output) &&
               eq(lhs.per_channel_levels.r.max_output, rhs.per_channel_levels.r.max_output) &&
               eq(lhs.per_channel_levels.g.min_input, rhs.per_channel_levels.g.min_input) &&
               eq(lhs.per_channel_levels.g.max_input, rhs.per_channel_levels.g.max_input) &&
               eq(lhs.per_channel_levels.g.gamma, rhs.per_channel_levels.g.gamma) &&
               eq(lhs.per_channel_levels.g.min_output, rhs.per_channel_levels.g.min_output) &&
               eq(lhs.per_channel_levels.g.max_output, rhs.per_channel_levels.g.max_output) &&
               eq(lhs.per_channel_levels.b.min_input, rhs.per_channel_levels.b.min_input) &&
               eq(lhs.per_channel_levels.b.max_input, rhs.per_channel_levels.b.max_input) &&
               eq(lhs.per_channel_levels.b.gamma, rhs.per_channel_levels.b.gamma) &&
               eq(lhs.per_channel_levels.b.min_output, rhs.per_channel_levels.b.min_output) &&
               eq(lhs.per_channel_levels.b.max_output, rhs.per_channel_levels.b.max_output) ||
           lhs.enable_geometry_modifiers == rhs.enable_geometry_modifiers;
}

bool operator!=(const image_transform& lhs, const image_transform& rhs) { return !(lhs == rhs); }

// audio_transform

audio_transform& audio_transform::operator*=(const audio_transform& other)
{
    volume *= other.volume;
    return *this;
}

audio_transform audio_transform::operator*(const audio_transform& other) const
{
    return audio_transform(*this) *= other;
}

audio_transform audio_transform::tween(double                 time,
                                       const audio_transform& source,
                                       const audio_transform& dest,
                                       double                 duration,
                                       const tweener&         tween)
{
    audio_transform result;
    result.volume = do_tween(time, source.volume, dest.volume, duration, tween);

    return result;
}

bool operator==(const audio_transform& lhs, const audio_transform& rhs) { return eq(lhs.volume, rhs.volume); }

bool operator!=(const audio_transform& lhs, const audio_transform& rhs) { return !(lhs == rhs); }

// frame_transform
frame_transform::frame_transform() = default;

frame_transform frame_transform::tween(double                 time,
                                       const frame_transform& source,
                                       const frame_transform& dest,
                                       double                 duration,
                                       const tweener&         tween)
{
    frame_transform result;
    result.image_transform =
        image_transform::tween(time, source.image_transform, dest.image_transform, duration, tween);
    result.audio_transform =
        audio_transform::tween(time, source.audio_transform, dest.audio_transform, duration, tween);
    return result;
}

bool operator==(const frame_transform& lhs, const frame_transform& rhs)
{
    return lhs.image_transform == rhs.image_transform && lhs.audio_transform == rhs.audio_transform;
}

bool operator!=(const frame_transform& lhs, const frame_transform& rhs) { return !(lhs == rhs); }

tweened_transform::tweened_transform(const frame_transform& source,
                                     const frame_transform& dest,
                                     int                    duration,
                                     tweener                tween)
    : source_(source)
    , dest_(dest)
    , duration_(duration)
    , tweener_(std::move(tween))
{
}

const frame_transform& tweened_transform::dest() const { return dest_; }

frame_transform tweened_transform::fetch()
{
    return time_ == duration_
               ? dest_
               : frame_transform::tween(
                     static_cast<double>(time_), source_, dest_, static_cast<double>(duration_), tweener_);
}

void tweened_transform::tick(int num) { time_ = std::min(time_ + num, duration_); }

std::optional<chroma::legacy_type> get_chroma_mode(const std::wstring& str)
{
    if (boost::iequals(str, L"none")) {
        return chroma::legacy_type::none;
    }
    if (boost::iequals(str, L"green")) {
        return chroma::legacy_type::green;
    }
    if (boost::iequals(str, L"blue")) {
        return chroma::legacy_type::blue;
    } else {
        return {};
    }
}

}} // namespace caspar::core
