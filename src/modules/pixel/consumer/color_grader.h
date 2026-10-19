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
 * Author: Dimitry Ishenko, dimitry (dot) ishenko (at) (gee) mail (dot) com
 */

#include "types.h"

#include <algorithm> // std::clamp
#include <array>
#include <cmath>
#include <tuple>

namespace caspar::pixel {

////////////////////
class color_grader
{
    coef coef_;
    std::array<level, 256> lut_;

    constexpr auto grade(pixel p) const {
        return std::tuple{ coef_.r * lut_[p.r], coef_.g * lut_[p.g], coef_.b * lut_[p.b] };
    }
    constexpr auto grade(level y) const { return coef_.w * lut_[y]; }

    static constexpr auto round(float v) { return std::clamp<level>(v + 0.5, 0, 255); }

public:
    constexpr color_grader() = default;
    constexpr color_grader(coef c, float γ)
    {
        // normalize on rgb, and scale w
        auto max = std::max({ c.r, c.g, c.b });
        coef_.r = c.r / max;
        coef_.g = c.g / max;
        coef_.b = c.b / max;
        coef_.w = c.w / max;

        // pre-compute gamma lut
        for (auto i = 0; i < 256; ++i) lut_[i] = round(255 * std::pow(i / 255.0, γ));
    }

    constexpr auto to_luma(pixel p) const
    {
        // bt709 luma weights
        auto y = round(0.2126 * p.r + 0.7152 * p.g + 0.0722 * p.b);
        return std::array{ lut_[y] };
    }

    constexpr auto to_rgb(pixel p) const
    {
        auto [r, g, b] = grade(p);
        return std::array{ round(r), round(g), round(b) };
    }

    constexpr auto to_rgbw(pixel p) const
    {
        auto y = std::min({ p.r, p.g, p.b });
        p.r -= y; p.g -= y; p.b -= y;

        auto [r, g, b] = grade(p);
        auto w = grade(y);
        return std::array{ round(r), round(g), round(b), round(w) };
    }

    constexpr auto to_rgbx(pixel p) const
    {
        auto [r, g, b] = grade(p);
        return std::array{ round(r), round(g), round(b), level{} };
    }
};

} // namespace caspar::pixel
