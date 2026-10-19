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

static const double epsilon = 0.001;

struct image_kernel::impl
{
    spl::shared_ptr<device> ogl_;
    spl::shared_ptr<shader> shader_;
    GLuint                  vao_;
    GLuint                  vbo_;

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

        // Color grading: ACES-based gamut/transfer/tonemapping pipeline.
        // Gamut index:  0=bt709, 1=bt2020, 2=dcip3_d65, 3=aces_ap0, 4=aces_ap1(acescg), 5=arri_wg3, 6=sgamut3_cine
        // Transfer:     0=linear, 1=srgb, 2=rec709, 3=pq, 4=hlg, 5=logc3, 6=slog3
        // Tonemapping:  0=none, 1=reinhard, 2=aces_filmic, 3=aces_rrt, 4=rrt_709, 5=rrt_p3, 6=rrt_2020_pq, 7=hlg_ootf
        {
            // From OpenColorIO 2.5.2 through the ACES studio config; each row reproduced
            // from published primaries (Bradford, or CAT02 for the two camera gamuts).
            static const float k_to_working[7][9] = {
                // bt709 -> ACEScg (AP1)
                {0.6130974f,  0.3395231f,  0.0473795f,  0.0701937f,  0.9163539f,  0.0134524f,  0.0206156f,  0.1095698f,  0.8698146f},
                // bt2020 -> ACEScg
                {0.9748950f,  0.0195991f,  0.0055059f,  0.0021796f,  0.9955355f,  0.0022850f,  0.0047972f,  0.0245320f,  0.9706708f},
                // dcip3 d65 -> ACEScg
                {0.7357979f,  0.2121665f,  0.0520356f,  0.0471799f,  0.9380457f,  0.0147744f,  0.0035637f,  0.0411419f,  0.9552944f},
                // aces_ap0 -> ACEScg (AP1)
                {1.4514393f, -0.2365108f, -0.2149286f, -0.0765538f,  1.1762297f, -0.0996759f,  0.0083161f, -0.0060324f,  0.9977163f},
                // aces_ap1 identity
                {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f},
                // arri wide gamut 3 -> ACEScg
                {0.9666334f,  0.1155416f, -0.0821751f,  0.0481904f,  1.1849383f, -0.2331287f,  0.0071933f, -0.0665937f,  1.0594004f},
                // sony sgamut3.cine -> ACEScg
                {0.9345170f,  0.1436417f, -0.0781587f, -0.0505267f,  1.2616092f, -0.2110825f, -0.0245030f, -0.0306710f,  1.0551741f}
            };
            static const float k_to_output[7][9] = {
                // ACEScg -> bt709
                { 1.7050509f, -0.6217921f, -0.0832589f, -0.1302564f,  1.1408048f, -0.0105483f, -0.0240034f, -0.1289690f,  1.1529723f},
                // ACEScg -> bt2020
                { 1.0258248f, -0.0200532f, -0.0057716f, -0.0022344f,  1.0045865f, -0.0023521f, -0.0050134f, -0.0252901f,  1.0303035f},
                // ACEScg -> dcip3 d65
                { 1.3792142f, -0.3088641f, -0.0703500f, -0.0693349f,  1.0822967f, -0.0129619f, -0.0021590f, -0.0454593f,  1.0476184f},
                // ACEScg -> aces_ap0
                {0.6954522f,  0.1406787f,  0.1638691f,  0.0447946f,  0.8596711f,  0.0955343f, -0.0055259f,  0.0040252f,  1.0015007f},
                // ACEScg identity (ap1)
                { 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f},
                // ACEScg -> arri wide gamut 3
                { 1.0389643f, -0.0979906f,  0.0590263f, -0.0441881f,  0.8586612f,  0.1855270f, -0.0098322f,  0.0546406f,  0.9551915f},
                // ACEScg -> sgamut3.cine
                { 1.0650269f, -0.1199250f,  0.0548981f,  0.0470203f,  0.7912176f,  0.1617622f,  0.0260986f,  0.0202137f,  0.9536877f}
            };
            const auto& cg = transforms.image_transform.color_grade;
            if (cg.enable) {
                int ig = std::min(std::max(cg.input_gamut, 0), 6);
                int og = std::min(std::max(cg.output_gamut, 0), 6);
                shader_->set("color_grading", true);
                shader_->set("input_transfer", cg.input_transfer);
                shader_->set("output_transfer", cg.output_transfer);
                shader_->set("tone_mapping_op", cg.tone_mapping);
                shader_->set("display_peak_luminance", 1000.0f);
                shader_->set("exposure", cg.exposure);

                // When no artistic tone mapping is applied and both gamuts are D65-based
                // (BT.709=0, BT.2020=1), use direct ITU-R BT.2087 matrices to avoid
                // chromatic adaptation artifacts from the ACEScg (D60) intermediate.
                static const float k_direct_cg[2][2][9] = {
                    { // from bt709
                        {1, 0, 0, 0, 1, 0, 0, 0, 1}, // -> bt709 (identity)
                        {0.6274039f, 0.3292830f, 0.0433131f, 0.0690972f, 0.9195404f, 0.0113623f, 0.0163914f, 0.0880133f, 0.8955953f}, // -> bt2020
                    },
                    { // from bt2020
                        {1.6604910f, -0.5876411f, -0.0728499f, -0.1245505f, 1.1328999f, -0.0083494f, -0.0181508f, -0.1005789f, 1.1187297f}, // -> bt709
                        {1, 0, 0, 0, 1, 0, 0, 0, 1}, // -> bt2020 (identity)
                    },
                };
                static const float k_identity_cg[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};

                if (cg.tone_mapping == 0 && ig <= 1 && og <= 1) {
                    // Direct D65<->D65 conversion -- no ACEScg intermediate needed
                    shader_->set_matrix3("input_to_working", k_direct_cg[ig][og]);
                    shader_->set_matrix3("working_to_output", k_identity_cg);
                } else {
                    // Full ACES grading pipeline through ACEScg working space
                    shader_->set_matrix3("input_to_working", k_to_working[ig]);
                    shader_->set_matrix3("working_to_output", k_to_output[og]);
                }

                // BT.2408 luminance adaptation: scale linear light when crossing
                // HDR/SDR domains.
                // For PQ (absolute): simple ratio 100/10000 or 10000/100.
                // For HLG (scene-referred, OOTF gamma=1.2): SDR white at 75% HLG
                // signal per BT.2408 section 3.2 -> scene-linear factor 0.265.
                auto get_luminance_scale = [](int src_t, int tgt_t) -> float {
                    constexpr float k_sdr_hlg = 0.265f; // BT.2408: 75% HLG signal for SDR ref white
                    bool            src_sdr   = (src_t <= 2); // 0=linear, 1=srgb, 2=rec709
                    bool            tgt_sdr   = (tgt_t <= 2);
                    bool            src_hlg   = (src_t == 4);
                    bool            tgt_hlg   = (tgt_t == 4);
                    bool            src_pq    = (src_t == 3);
                    bool            tgt_pq    = (tgt_t == 3);
                    if (src_sdr && tgt_hlg)
                        return k_sdr_hlg; // SDR -> HLG
                    if (src_hlg && tgt_sdr)
                        return 1.0f / k_sdr_hlg; // HLG -> SDR (3.774)
                    if (src_sdr && tgt_pq)
                        return 0.01f; // SDR -> PQ (100/10000)
                    if (src_pq && tgt_sdr)
                        return 100.0f; // PQ -> SDR
                    if (src_hlg && tgt_pq)
                        return 0.1f; // HLG -> PQ (1000/10000)
                    if (src_pq && tgt_hlg)
                        return 10.0f; // PQ -> HLG
                    return 1.0f;      // same domain
                };
                shader_->set("luminance_scale", get_luminance_scale(cg.input_transfer, cg.output_transfer));
            } else {
                shader_->set("color_grading", false);
            }
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
