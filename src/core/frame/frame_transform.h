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

#include <common/tweener.h>

#include <core/mixer/image/blend_modes.h>

#include <array>
#include <optional>

namespace caspar { namespace core {

struct chroma
{
    enum class legacy_type
    {
        none,
        green,
        blue
    };

    bool   enable                    = false;
    bool   show_mask                 = false;
    double target_hue                = 0.0;
    double hue_width                 = 0.0;
    double min_saturation            = 0.0;
    double min_brightness            = 0.0;
    double softness                  = 0.0;
    double spill_suppress            = 0.0;
    double spill_suppress_saturation = 1.0;
};

struct levels final
{
    double min_input  = 0.0;
    double max_input  = 1.0;
    double gamma      = 1.0;
    double min_output = 0.0;
    double max_output = 1.0;
};

struct corners final
{
    std::array<double, 2> ul = {0.0, 0.0};
    std::array<double, 2> ur = {1.0, 0.0};
    std::array<double, 2> lr = {1.0, 1.0};
    std::array<double, 2> ll = {0.0, 1.0};
};

struct rectangle final
{
    std::array<double, 2> ul = {0.0, 0.0};
    std::array<double, 2> lr = {1.0, 1.0};
};

/// An inclusive range for a grading parameter, and the clamp that enforces it.
struct grade_range final
{
    double lo;
    double hi;

    [[nodiscard]] constexpr bool contains(double v) const
    {
        // Written as a positive test so a NaN falls outside: every comparison against
        // NaN is false. std::stod("nan") succeeds, so without this a MIXER command
        // could put a NaN into the transform and every pixel it touched would go black.
        return v >= lo && v <= hi;
    }

    [[nodiscard]] constexpr double clamp(double v) const
    {
        if (!(v >= lo)) // also catches NaN, which must not survive as itself
            return lo;
        return v > hi ? hi : v;
    }
};

/// Legal ranges for the primary grading parameters.
///
/// One table, used twice: the MIXER commands refuse anything outside it, and combining
/// transforms clamps the result back into it. Both have to agree, or a value no single
/// command could set becomes reachable by stacking two layers.
///
/// The bounds are deliberately generous rather than tasteful. They exist to keep the
/// arithmetic defined and to stop a typo from blacking out a layer, not to enforce a
/// look, so they sit well outside what a grade would normally use. Where a standard
/// gives one it is followed: ASC CDL requires slope >= 0, power > 0 and saturation >= 0.
namespace grade_limits {

inline constexpr grade_range temperature{-1.0, 1.0};
inline constexpr grade_range tint{-1.0, 1.0};
inline constexpr grade_range lift{-1.0, 1.0};
/// The shader raises to 1/midtone, so zero is a division by zero and negatives invert
/// the curve. Matches the clamp in apply_lmg.
inline constexpr grade_range midtone{0.01, 100.0};
inline constexpr grade_range gain{0.0, 10.0};
/// Rotation is periodic, so this is a wrap rather than a clamp -- see
/// apply_transform_colour_values. 200 degrees is -160, not 180.
inline constexpr grade_range hue_shift{-180.0, 180.0};
inline constexpr grade_range tone{-1.0, 1.0}; // shadows / highlights
inline constexpr grade_range split_color{-1.0, 1.0};
inline constexpr grade_range split_balance{0.0, 1.0};
inline constexpr grade_range cdl_slope{0.0, 10.0};
inline constexpr grade_range cdl_offset{-1.0, 1.0};
inline constexpr grade_range cdl_power{0.01, 10.0};
inline constexpr grade_range cdl_saturation{0.0, 10.0};

/// Blur radius is in output pixels; the bound stops a typo stalling a layer.
/// Normalised position or fraction of the frame.
inline constexpr grade_range unit{0.0, 1.0};
inline constexpr grade_range blur_radius{0.0, 200.0};
inline constexpr grade_range blur_angle{-360.0, 360.0};
inline constexpr grade_range sharpen_amount{0.0, 5.0};
/// Multiplier on the texel step; 0 would sample the centre four times.
inline constexpr grade_range sharpen_radius{0.1, 10.0};
inline constexpr grade_range grain_intensity{0.0, 1.0};
/// The shader floors this at 0.5 when dividing by it.
inline constexpr grade_range grain_size{0.5, 10.0};

} // namespace grade_limits
enum class blur_type : int
{
    gaussian    = 0,
    box         = 1,
    directional = 2,
    zoom        = 3,
    tilt_shift  = 4,
    lens        = 5
};

struct blur_config final
{
    bool                  enable = false;
    double                radius = 0.0;
    blur_type             type   = blur_type::gaussian;
    double                angle  = 0.0;
    std::array<double, 2> center = {0.5, 0.5};
    double                tilt_y = 0.5;
    double                tilt_h = 0.2;
};

struct image_transform final
{
    double opacity    = 1.0;
    double contrast   = 1.0;
    double brightness = 1.0;
    double saturation = 1.0;

    /**
     * This enables the clip/crop/perspective fields.
     * It is often desirable to have this disabled, to avoid cropping/clipping unnecessarily
     */
    bool enable_geometry_modifiers = false;

    std::array<double, 2> anchor           = {0.0, 0.0};
    std::array<double, 2> fill_translation = {0.0, 0.0};
    std::array<double, 2> fill_scale       = {1.0, 1.0};
    std::array<double, 2> clip_translation = {0.0, 0.0};
    std::array<double, 2> clip_scale       = {1.0, 1.0};
    double                angle            = 0.0;
    rectangle             crop;
    corners               perspective;
    core::levels          levels;
    core::chroma          chroma;
    double                temperature = 0.0; // white balance: -1 cool .. +1 warm
    double                tint        = 0.0; // white balance: -1 magenta .. +1 green
    std::array<double, 3> lift    = {0.0, 0.0, 0.0}; // shadow offset per channel
    std::array<double, 3> midtone = {1.0, 1.0, 1.0}; // midtone power per channel (DaVinci "gamma")
    std::array<double, 3> gain    = {1.0, 1.0, 1.0}; // highlight multiplier per channel
    double                hue_shift = 0.0; // global hue rotation, degrees -180..+180
    double                shadows    = 0.0; // tonal balance: shadow region lift/cut -1..+1
    double                highlights = 0.0; // tonal balance: highlight region lift/cut -1..+1
    std::array<double, 3> split_shadow_color    = {0.0, 0.0, 0.0}; // split tone: shadow tint RGB
    std::array<double, 3> split_highlight_color = {0.0, 0.0, 0.0}; // split tone: highlight tint RGB
    double                split_balance         = 0.5;             // split tone: crossover 0..1
    std::array<double, 3> cdl_slope      = {1.0, 1.0, 1.0}; // ASC CDL slope
    std::array<double, 3> cdl_offset     = {0.0, 0.0, 0.0}; // ASC CDL offset
    std::array<double, 3> cdl_power      = {1.0, 1.0, 1.0}; // ASC CDL power
    double                cdl_saturation = 1.0;             // ASC CDL saturation
    core::blur_config     blur;
    double                sharpen_amount = 0.0;
    double                sharpen_radius = 1.0;
    double                grain_intensity = 0.0;
    double                grain_size      = 1.0;

    bool             is_key      = false;
    bool             invert      = false;
    bool             is_mix      = false;
    core::blend_mode blend_mode  = blend_mode::normal;
    int              layer_depth = 0;

    static image_transform tween(double                 time,
                                 const image_transform& source,
                                 const image_transform& dest,
                                 double                 duration,
                                 const tweener&         tween);
};

bool operator==(const image_transform& lhs, const image_transform& rhs);
bool operator!=(const image_transform& lhs, const image_transform& rhs);

struct audio_transform final
{
    double volume           = 1.0;
    bool   immediate_volume = false; // When false, intra-frame samples are ramped from previous volume in audio mixer

    audio_transform& operator*=(const audio_transform& other);
    audio_transform  operator*(const audio_transform& other) const;

    static audio_transform tween(double                 time,
                                 const audio_transform& source,
                                 const audio_transform& dest,
                                 double                 duration,
                                 const tweener&         tween);
};

bool operator==(const audio_transform& lhs, const audio_transform& rhs);
bool operator!=(const audio_transform& lhs, const audio_transform& rhs);

struct frame_transform final
{
  public:
    frame_transform();

    core::image_transform image_transform;
    core::audio_transform audio_transform;

    static frame_transform tween(double                 time,
                                 const frame_transform& source,
                                 const frame_transform& dest,
                                 double                 duration,
                                 const tweener&         tween);
};

bool operator==(const frame_transform& lhs, const frame_transform& rhs);
bool operator!=(const frame_transform& lhs, const frame_transform& rhs);

class tweened_transform
{
    frame_transform source_;
    frame_transform dest_;
    int             duration_ = 0;
    int             time_     = 0;
    tweener         tweener_;

  public:
    tweened_transform() = default;

    tweened_transform(const frame_transform& source, const frame_transform& dest, int duration, tweener tween);

    const frame_transform& dest() const;

    frame_transform fetch();
    void            tick(int num);
};

std::optional<chroma::legacy_type> get_chroma_mode(const std::wstring& str);

}} // namespace caspar::core
