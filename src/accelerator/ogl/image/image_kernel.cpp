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
#include "image_kernel.h"

#include "image_shader.h"

#include "../util/device.h"
#include "../util/shader.h"
#include "../util/texture.h"

#include <common/assert.h>
#include <common/gl/gl_check.h>

#include <core/frame/frame_transform.h>
#include <core/frame/pixel_format.h>

#include <boost/algorithm/cxx11/all_of.hpp>
#include <boost/range/adaptor/transformed.hpp>

#include <GL/glew.h>

#include <array>
#include <cmath>
#include <algorithm>
#include <utility>
#include <vector>

namespace caspar::accelerator::ogl {

double get_precision_factor(common::bit_depth depth)
{
    switch (depth) {
        case common::bit_depth::bit8:
            return 1.0;
        case common::bit_depth::bit10:
            return 64.0;
        case common::bit_depth::bit12:
            return 16.0;
        case common::bit_depth::bit16:
            return 1.0;
        default:
            return 1.0;
    }
}

bool is_above_screen(double y) { return y < 0.0; }

bool is_below_screen(double y) { return y > 1.0; }

bool is_left_of_screen(double x) { return x < 0.0; }

bool is_right_of_screen(double x) { return x > 1.0; }

bool is_outside_screen(const std::vector<core::frame_geometry::coord>& coords)
{
    auto x_coords =
        coords | boost::adaptors::transformed([](const core::frame_geometry::coord& c) { return c.vertex_x; });
    auto y_coords =
        coords | boost::adaptors::transformed([](const core::frame_geometry::coord& c) { return c.vertex_y; });

    return boost::algorithm::all_of(x_coords, &is_left_of_screen) ||
           boost::algorithm::all_of(x_coords, &is_right_of_screen) ||
           boost::algorithm::all_of(y_coords, &is_above_screen) || boost::algorithm::all_of(y_coords, &is_below_screen);
}

// Builds a 256-entry 1D LUT from control points using Fritsch-Carlson monotone
// cubic Hermite interpolation. Guarantees no overshoot (safe for colour values).
// If fewer than 2 points, returns a linear identity LUT.
static std::array<float, 256> build_curve_lut(const core::curve_channel& cc)
{
    std::array<float, 256> lut;
    if (cc.count < 2) {
        for (int i = 0; i < 256; ++i)
            lut[i] = i / 255.0f;
        return lut;
    }
    std::vector<std::pair<double, double>> pts;
    pts.reserve(cc.count);
    for (int i = 0; i < cc.count; ++i)
        pts.push_back({cc.points[i].x, cc.points[i].y});
    std::sort(pts.begin(), pts.end());

    int                 n = static_cast<int>(pts.size());
    std::vector<double> dx(n - 1), dy(n - 1), delta(n - 1), m(n);
    for (int i = 0; i < n - 1; ++i) {
        dx[i]    = pts[i + 1].first - pts[i].first;
        dy[i]    = pts[i + 1].second - pts[i].second;
        delta[i] = (dx[i] > 1e-10) ? dy[i] / dx[i] : 0.0;
    }
    m[0]     = delta[0];
    m[n - 1] = delta[n - 2];
    for (int i = 1; i < n - 1; ++i)
        m[i] = (delta[i - 1] + delta[i]) * 0.5;
    for (int i = 0; i < n - 1; ++i) {
        if (std::abs(delta[i]) < 1e-10) {
            m[i] = m[i + 1] = 0.0;
            continue;
        }
        double a = m[i] / delta[i];
        double b = m[i + 1] / delta[i];
        double h = std::sqrt(a * a + b * b);
        if (h > 3.0) {
            m[i] *= 3.0 / h;
            m[i + 1] *= 3.0 / h;
        }
    }
    for (int k = 0; k < 256; ++k) {
        double t = k / 255.0;
        if (t <= pts.front().first) {
            lut[k] = static_cast<float>(std::max(0.0, std::min(1.0, pts.front().second)));
            continue;
        }
        if (t >= pts.back().first) {
            lut[k] = static_cast<float>(std::max(0.0, std::min(1.0, pts.back().second)));
            continue;
        }
        // n - 1, not n - 2: there are n-1 intervals between n control points, and the
        // search has to be able to reach the last one. With n - 2 the interval
        // [pts[n-2], pts[n-1]) matched no iteration, seg kept its initial 0, and the
        // final stretch of every curve with three or more points was evaluated with
        // the FIRST segment's control points and tangents at a parameter far outside
        // [0,1].
        int seg = 0;
        for (int i = 0; i < n - 1; ++i)
            if (t >= pts[i].first && t < pts[i + 1].first) {
                seg = i;
                break;
            }
        double h_  = dx[seg];
        double t_  = (h_ > 1e-10) ? (t - pts[seg].first) / h_ : 0.0;
        double t2  = t_ * t_;
        double t3  = t2 * t_;
        double h00 = 2 * t3 - 3 * t2 + 1;
        double h10 = t3 - 2 * t2 + t_;
        double h01 = -2 * t3 + 3 * t2;
        double h11 = t3 - t2;
        double val = h00 * pts[seg].second + h10 * h_ * m[seg] + h01 * pts[seg + 1].second + h11 * h_ * m[seg + 1];
        lut[k]     = static_cast<float>(std::max(0.0, std::min(1.0, val)));
    }
    return lut;
}

static const double epsilon = 0.001;

struct image_kernel::impl
{
    spl::shared_ptr<device> ogl_;
    spl::shared_ptr<shader> shader_;
    GLuint                  vao_;
    GLuint                  vbo_;
    GLuint                  lut3d_tex_id_   = 0;
    const core::lut3d_data* lut3d_data_ptr_ = nullptr; // tracks which LUT data is uploaded
    GLuint                  curve_lut_tex_id_ = 0;
    GLuint                  hue_curve_tex_id_ = 0;

    explicit impl(const spl::shared_ptr<device>& ogl)
        : ogl_(ogl)
        , shader_(ogl_->dispatch_sync([&] { return get_image_shader(ogl); }))
    {
        ogl_->dispatch_sync([&] {
            GL(glGenVertexArrays(1, &vao_));
            GL(glGenBuffers(1, &vbo_));
        });
    }

    ~impl()
    {
        ogl_->dispatch_sync([&] {
            GL(glDeleteVertexArrays(1, &vao_));
            GL(glDeleteBuffers(1, &vbo_));
            if (lut3d_tex_id_)
                GL(glDeleteTextures(1, &lut3d_tex_id_));
            if (curve_lut_tex_id_)
                GL(glDeleteTextures(1, &curve_lut_tex_id_));
            if (hue_curve_tex_id_)
                GL(glDeleteTextures(1, &hue_curve_tex_id_));
        });
    }

    void draw(draw_params params)
    {
        CASPAR_ASSERT(params.pix_desc.planes.size() == params.textures.size());

        if (params.textures.empty() || !params.background) {
            return;
        }

        if (params.transforms.image_transform.opacity < epsilon) {
            return;
        }

        auto coords = params.geometry.data();
        if (coords.empty()) {
            return;
        }

        auto transforms = params.transforms;

        auto const first_plane = params.pix_desc.planes.at(0);
        if (params.geometry.mode() != core::frame_geometry::scale_mode::stretch && first_plane.width > 0 &&
            first_plane.height > 0) {
            auto width_scale  = static_cast<double>(params.target_width) / static_cast<double>(first_plane.width);
            auto height_scale = static_cast<double>(params.target_height) / static_cast<double>(first_plane.height);

            core::image_transform transform;
            double                target_scale;
            switch (params.geometry.mode()) {
                case core::frame_geometry::scale_mode::fit:
                    target_scale = std::min(width_scale, height_scale);

                    transform.fill_scale[0] *= target_scale / width_scale;
                    transform.fill_scale[1] *= target_scale / height_scale;
                    break;

                case core::frame_geometry::scale_mode::fill:
                    target_scale = std::max(width_scale, height_scale);
                    transform.fill_scale[0] *= target_scale / width_scale;
                    transform.fill_scale[1] *= target_scale / height_scale;
                    break;

                case core::frame_geometry::scale_mode::original:
                    transform.fill_scale[0] /= width_scale;
                    transform.fill_scale[1] /= height_scale;
                    break;

                case core::frame_geometry::scale_mode::hfill:
                    transform.fill_scale[1] *= width_scale / height_scale;
                    break;

                case core::frame_geometry::scale_mode::vfill:
                    transform.fill_scale[0] *= height_scale / width_scale;
                    break;

                default:;
            }

            transforms = transforms.combine_transform(transform, params.aspect_ratio);
        }

        coords = transforms.transform_coords(coords);

        // Skip drawing if all the coordinates will be outside the screen.
        if (coords.size() < 3 || is_outside_screen(coords)) {
            return;
        }

        double precision_factor[4] = {1, 1, 1, 1};

        // Bind textures

        for (int n = 0; n < params.textures.size(); ++n) {
            params.textures[n]->bind(n);
            precision_factor[n] = get_precision_factor(params.textures[n]->depth());
        }

        if (params.local_key) {
            params.local_key->bind(static_cast<int>(texture_id::local_key));
        }

        if (params.layer_key) {
            params.layer_key->bind(static_cast<int>(texture_id::layer_key));
        }

        const auto color_space = core::decode_color_space(params.pix_desc);

        // Row order is R, G, B; columns are Y, Cb, Cr. Each row follows from the
        // luma coefficients: Cr->R is 2(1-Kr), Cb->B is 2(1-Kb), and the two green terms
        // are -2(1-Kb)Kb/Kg and -2(1-Kr)Kr/Kg.
        //
        // The BT.601 Cr->G term was -0.509 and should be -0.714136:
        // -2(1-0.299)(0.299)/0.587. BT.709 and BT.2020 were both already correct, which
        // is what made this hard to see -- only SD material was affected, and only in
        // green. Measured on flat patches decoded as BT.601, green was out by up to 20
        // LSB on saturated colour while red and blue were exact.
        const float color_matrices[3][9] = {
            {1.0, 0.0, 1.402, 1.0, -0.344136, -0.714136, 1.0, 1.772, 0.0},                    // bt.601
            {1.0, 0.0, 1.5748, 1.0, -0.1873, -0.4681, 1.0, 1.8556, 0.0},                      // bt.709
            {1.0, 0.0, 1.4746, 1.0, -0.16455312684366, -0.57135312684366, 1.0, 1.8814, 0.0}}; // bt.2020
        const auto color_matrix = color_matrices[static_cast<int>(color_space)];

        const float luma_coefficients[3][3] = {{0.299, 0.587, 0.114},     // bt.601
                                               {0.2126, 0.7152, 0.0722},  // bt.709
                                               {0.2627, 0.6780, 0.0593}}; // bt.2020
        const auto  luma_coeff              = luma_coefficients[static_cast<int>(color_space)];

        // Setup shader
        shader_->use();

        shader_->set("is_straight_alpha", params.pix_desc.is_straight_alpha);
        shader_->set("plane[0]", texture_id::plane0);
        shader_->set("plane[1]", texture_id::plane1);
        shader_->set("plane[2]", texture_id::plane2);
        shader_->set("plane[3]", texture_id::plane3);
        shader_->set("precision_factor[0]", precision_factor[0]);
        shader_->set("precision_factor[1]", precision_factor[1]);
        shader_->set("precision_factor[2]", precision_factor[2]);
        shader_->set("precision_factor[3]", precision_factor[3]);
        shader_->set("local_key", texture_id::local_key);
        shader_->set("layer_key", texture_id::layer_key);
        shader_->set_matrix3("color_matrix", color_matrix);
        shader_->set("luma_coeff", luma_coeff[0], luma_coeff[1], luma_coeff[2]);
        shader_->set("has_local_key", static_cast<bool>(params.local_key));
        shader_->set("has_layer_key", static_cast<bool>(params.layer_key));
        shader_->set("pixel_format", params.pix_desc.format);
        shader_->set("opacity", transforms.image_transform.is_key ? 1.0 : transforms.image_transform.opacity);

        if (transforms.image_transform.chroma.enable) {
            shader_->set("chroma", true);
            shader_->set("chroma_show_mask", transforms.image_transform.chroma.show_mask);
            shader_->set("chroma_target_hue", transforms.image_transform.chroma.target_hue / 360.0);
            shader_->set("chroma_hue_width", transforms.image_transform.chroma.hue_width);
            shader_->set("chroma_min_saturation", transforms.image_transform.chroma.min_saturation);
            shader_->set("chroma_min_brightness", transforms.image_transform.chroma.min_brightness);
            shader_->set("chroma_softness", 1.0 + transforms.image_transform.chroma.softness);
            shader_->set("chroma_spill_suppress", transforms.image_transform.chroma.spill_suppress / 360.0);
            shader_->set("chroma_spill_suppress_saturation",
                         transforms.image_transform.chroma.spill_suppress_saturation);
        } else {
            shader_->set("chroma", false);
        }

        // Setup blend_func

        if (transforms.image_transform.is_key) {
            params.blend_mode = core::blend_mode::normal;
        }

        params.background->bind(static_cast<int>(texture_id::background));
        shader_->set("background", texture_id::background);
        shader_->set("blend_mode", params.blend_mode);
        shader_->set("keyer", params.keyer);

        // Setup image-adjustments
        shader_->set("invert", transforms.image_transform.invert);

        if (transforms.image_transform.levels.min_input > epsilon ||
            transforms.image_transform.levels.max_input < 1.0 - epsilon ||
            transforms.image_transform.levels.min_output > epsilon ||
            transforms.image_transform.levels.max_output < 1.0 - epsilon ||
            std::abs(transforms.image_transform.levels.gamma - 1.0) > epsilon) {
            shader_->set("levels", true);
            shader_->set("min_input", transforms.image_transform.levels.min_input);
            shader_->set("max_input", transforms.image_transform.levels.max_input);
            shader_->set("min_output", transforms.image_transform.levels.min_output);
            shader_->set("max_output", transforms.image_transform.levels.max_output);
            shader_->set("gamma", transforms.image_transform.levels.gamma);
        } else {
            shader_->set("levels", false);
        }

        if (std::abs(transforms.image_transform.brightness - 1.0) > epsilon ||
            std::abs(transforms.image_transform.saturation - 1.0) > epsilon ||
            std::abs(transforms.image_transform.contrast - 1.0) > epsilon) {
            shader_->set("csb", true);

            shader_->set("brt", transforms.image_transform.brightness);
            shader_->set("sat", transforms.image_transform.saturation);
            shader_->set("con", transforms.image_transform.contrast);
        } else {
            shader_->set("csb", false);
        }

        // White balance (temperature / tint)
        if (std::abs(transforms.image_transform.temperature) > epsilon ||
            std::abs(transforms.image_transform.tint) > epsilon) {
            shader_->set("white_balance", true);
            shader_->set("wb_temperature", static_cast<float>(transforms.image_transform.temperature));
            shader_->set("wb_tint", static_cast<float>(transforms.image_transform.tint));
        } else {
            shader_->set("white_balance", false);
        }

        // Lift / Midtone / Gain (per-channel 3-way colour corrector)
        {
            const auto& lift    = transforms.image_transform.lift;
            const auto& midtone = transforms.image_transform.midtone;
            const auto& gain    = transforms.image_transform.gain;
            bool        lmg_active =
                std::abs(lift[0]) > epsilon || std::abs(lift[1]) > epsilon || std::abs(lift[2]) > epsilon ||
                std::abs(midtone[0] - 1.0) > epsilon || std::abs(midtone[1] - 1.0) > epsilon ||
                std::abs(midtone[2] - 1.0) > epsilon || std::abs(gain[0] - 1.0) > epsilon ||
                std::abs(gain[1] - 1.0) > epsilon || std::abs(gain[2] - 1.0) > epsilon;
            if (lmg_active) {
                shader_->set("lmg_enable", true);
                shader_->set("lmg_lift", lift[0], lift[1], lift[2]);
                shader_->set("lmg_midtone", midtone[0], midtone[1], midtone[2]);
                shader_->set("lmg_gain", gain[0], gain[1], gain[2]);
            } else {
                shader_->set("lmg_enable", false);
            }
        }

        // Hue shift
        if (std::abs(transforms.image_transform.hue_shift) > epsilon) {
            shader_->set("hue_shift_enable", true);
            shader_->set("hue_shift_degrees", static_cast<float>(transforms.image_transform.hue_shift));
        } else {
            shader_->set("hue_shift_enable", false);
        }

        // Tonal balance (shadows / highlights)
        if (std::abs(transforms.image_transform.shadows) > epsilon ||
            std::abs(transforms.image_transform.highlights) > epsilon) {
            shader_->set("tonebalance_enable", true);
            shader_->set("tb_shadows", static_cast<float>(transforms.image_transform.shadows));
            shader_->set("tb_highlights", static_cast<float>(transforms.image_transform.highlights));
        } else {
            shader_->set("tonebalance_enable", false);
        }

        // Split toning (independent shadow / highlight tint)
        {
            const auto& sc = transforms.image_transform.split_shadow_color;
            const auto& hc = transforms.image_transform.split_highlight_color;
            bool        split_active =
                std::abs(sc[0]) > epsilon || std::abs(sc[1]) > epsilon || std::abs(sc[2]) > epsilon ||
                std::abs(hc[0]) > epsilon || std::abs(hc[1]) > epsilon || std::abs(hc[2]) > epsilon;
            if (split_active) {
                shader_->set("split_tone_enable", true);
                shader_->set("split_shadow_color", sc[0], sc[1], sc[2]);
                shader_->set("split_highlight_color", hc[0], hc[1], hc[2]);
                shader_->set("split_balance", static_cast<float>(transforms.image_transform.split_balance));
            } else {
                shader_->set("split_tone_enable", false);
            }
        }

        // ASC CDL (Slope/Offset/Power)
        {
            const auto& s  = transforms.image_transform.cdl_slope;
            const auto& o  = transforms.image_transform.cdl_offset;
            const auto& p  = transforms.image_transform.cdl_power;
            double      cs = transforms.image_transform.cdl_saturation;
            bool        cdl_active =
                std::abs(s[0] - 1.0) > epsilon || std::abs(s[1] - 1.0) > epsilon || std::abs(s[2] - 1.0) > epsilon ||
                std::abs(o[0]) > epsilon || std::abs(o[1]) > epsilon || std::abs(o[2]) > epsilon ||
                std::abs(p[0] - 1.0) > epsilon || std::abs(p[1] - 1.0) > epsilon || std::abs(p[2] - 1.0) > epsilon ||
                std::abs(cs - 1.0) > epsilon;
            if (cdl_active) {
                shader_->set("cdl_enable", true);
                shader_->set("cdl_slope", s[0], s[1], s[2]);
                shader_->set("cdl_offset", o[0], o[1], o[2]);
                shader_->set("cdl_power", p[0], p[1], p[2]);
                shader_->set("cdl_saturation", static_cast<float>(cs));
            } else {
                shader_->set("cdl_enable", false);
            }
        }

        // 3D LUT
        {
            const auto& lut = transforms.image_transform.lut3d;
            if (lut && lut->size > 0 && !lut->data.empty()) {
                // Re-upload only when the data pointer changes (a new LUT was loaded).
                if (lut.get() != lut3d_data_ptr_) {
                    if (lut3d_tex_id_)
                        GL(glDeleteTextures(1, &lut3d_tex_id_));
                    GL(glCreateTextures(GL_TEXTURE_3D, 1, &lut3d_tex_id_));
                    GL(glTextureStorage3D(lut3d_tex_id_, 1, GL_RGB32F, lut->size, lut->size, lut->size));
                    GL(glTextureParameteri(lut3d_tex_id_, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
                    GL(glTextureParameteri(lut3d_tex_id_, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
                    GL(glTextureParameteri(lut3d_tex_id_, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
                    GL(glTextureParameteri(lut3d_tex_id_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
                    GL(glTextureParameteri(lut3d_tex_id_, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE));
                    GL(glTextureSubImage3D(lut3d_tex_id_, 0, 0, 0, 0,
                                           lut->size, lut->size, lut->size,
                                           GL_RGB, GL_FLOAT, lut->data.data()));
                    lut3d_data_ptr_ = lut.get();
                }
                GL(glBindTextureUnit(static_cast<int>(texture_id::lut3d_tex), lut3d_tex_id_));
                shader_->set("lut3d_enable", true);
                shader_->set("lut3d_tex", static_cast<int>(texture_id::lut3d_tex));
                shader_->set("lut3d_strength", transforms.image_transform.lut3d_strength);
            } else {
                shader_->set("lut3d_enable", false);
                if (lut3d_tex_id_ && !lut)
                    lut3d_data_ptr_ = nullptr;
            }
        }

        // Tone curves: build 256-entry LUTs on the CPU, pack into an RGBA32F 256x1 texture.
        {
            const auto& cv = transforms.image_transform.curves;
            if (cv.enable) {
                auto lut_r = build_curve_lut(cv.red);
                auto lut_g = build_curve_lut(cv.green);
                auto lut_b = build_curve_lut(cv.blue);
                auto lut_m = build_curve_lut(cv.master);
                std::vector<float> rgba(256 * 4);
                for (int i = 0; i < 256; ++i) {
                    // BGR working order: .r slot = displayed Blue, .b slot = displayed Red.
                    rgba[i * 4 + 0] = lut_b[i];
                    rgba[i * 4 + 1] = lut_g[i];
                    rgba[i * 4 + 2] = lut_r[i];
                    rgba[i * 4 + 3] = lut_m[i];
                }
                if (!curve_lut_tex_id_) {
                    GL(glCreateTextures(GL_TEXTURE_2D, 1, &curve_lut_tex_id_));
                    GL(glTextureStorage2D(curve_lut_tex_id_, 1, GL_RGBA32F, 256, 1));
                    GL(glTextureParameteri(curve_lut_tex_id_, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
                    GL(glTextureParameteri(curve_lut_tex_id_, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
                    GL(glTextureParameteri(curve_lut_tex_id_, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
                    GL(glTextureParameteri(curve_lut_tex_id_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
                }
                GL(glTextureSubImage2D(curve_lut_tex_id_, 0, 0, 0, 256, 1, GL_RGBA, GL_FLOAT, rgba.data()));
                GL(glBindTextureUnit(static_cast<int>(texture_id::curve_lut_tex), curve_lut_tex_id_));
                shader_->set("curves_enable", true);
                shader_->set("curve_lut_tex", static_cast<int>(texture_id::curve_lut_tex));
            } else {
                shader_->set("curves_enable", false);
            }
        }

        // Hue-vs-Hue / Hue-vs-Sat curves
        {
            const auto& hc = transforms.image_transform.hue_curves;
            if (hc && !hc->data.empty()) {
                if (!hue_curve_tex_id_) {
                    GL(glCreateTextures(GL_TEXTURE_2D, 1, &hue_curve_tex_id_));
                    GL(glTextureStorage2D(hue_curve_tex_id_, 1, GL_RGBA32F, 256, 1));
                    GL(glTextureParameteri(hue_curve_tex_id_, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
                    GL(glTextureParameteri(hue_curve_tex_id_, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
                    GL(glTextureParameteri(hue_curve_tex_id_, GL_TEXTURE_WRAP_S, GL_REPEAT));
                    GL(glTextureParameteri(hue_curve_tex_id_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
                }
                GL(glTextureSubImage2D(hue_curve_tex_id_, 0, 0, 0, 256, 1, GL_RGBA, GL_FLOAT, hc->data.data()));
                GL(glBindTextureUnit(static_cast<int>(texture_id::hue_curve_tex), hue_curve_tex_id_));
                shader_->set("hue_curve_enable", true);
                shader_->set("hue_curve_tex", static_cast<int>(texture_id::hue_curve_tex));
            } else {
                shader_->set("hue_curve_enable", false);
            }
        }

        // Setup drawing area

        GL(glViewport(0, 0, params.background->width(), params.background->height()));
        glDisable(GL_DEPTH_TEST);

        // Set render target
        params.background->attach();

        // Draw
        GL(glBindVertexArray(vao_));
        GL(glBindBuffer(GL_ARRAY_BUFFER, vbo_));

        GL(glBufferData(GL_ARRAY_BUFFER,
                        static_cast<GLsizeiptr>(sizeof(core::frame_geometry::coord)) * coords.size(),
                        coords.data(),
                        GL_STATIC_DRAW));

        auto stride = static_cast<GLsizei>(sizeof(core::frame_geometry::coord));

        auto vtx_loc = shader_->get_attrib_location("Position");
        auto tex_loc = shader_->get_attrib_location("TexCoordIn");

        GL(glEnableVertexAttribArray(vtx_loc));
        GL(glEnableVertexAttribArray(tex_loc));

        GL(glVertexAttribPointer(vtx_loc, 2, GL_DOUBLE, GL_FALSE, stride, nullptr));
        GL(glVertexAttribPointer(tex_loc, 4, GL_DOUBLE, GL_FALSE, stride, (GLvoid*)(2 * sizeof(GLdouble))));

        GL(glDrawArrays(GL_TRIANGLE_FAN, 0, static_cast<GLsizei>(coords.size())));
        GL(glTextureBarrier());

        GL(glDisableVertexAttribArray(vtx_loc));
        GL(glDisableVertexAttribArray(tex_loc));

        GL(glBindVertexArray(0));
        GL(glBindBuffer(GL_ARRAY_BUFFER, 0));

        // Cleanup
        GL(glDisable(GL_SCISSOR_TEST));
        GL(glDisable(GL_BLEND));
    }
};

image_kernel::image_kernel(const spl::shared_ptr<device>& ogl)
    : impl_(new impl(ogl))
{
}
image_kernel::~image_kernel() {}
void image_kernel::draw(const draw_params& params) { impl_->draw(params); }

} // namespace caspar::accelerator::ogl
