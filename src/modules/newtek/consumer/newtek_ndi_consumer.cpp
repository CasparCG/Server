/*
 * Copyright 2018
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
 * Author: Krzysztof Zegzula, zegzulakrzysztof@gmail.com
 * based on work of Robert Nagy, ronag89@gmail.com
 */

#include "../StdAfx.h"

#include "newtek_ndi_consumer.h"

#include <core/consumer/frame_consumer.h>
#include <core/frame/frame.h>
#include <core/mixer/audio/audio_util.h>
#include <core/video_format.h>

#include <common/assert.h>
#include <common/diagnostics/graph.h>
#include <common/future.h>
#include <common/param.h>
#include <common/timer.h>
#include <common/executor.h>

#include <boost/algorithm/string.hpp>
#include <boost/property_tree/ptree.hpp>

#include "../util/ndi.h"

namespace caspar { namespace newtek {

struct newtek_ndi_consumer : public core::frame_consumer
{
    static std::atomic<int> instances_;
    const int               instance_no_;
    const std::wstring      name_;
    const bool              allow_fields_;
    const NDIlib_FourCC_type_e video_format_;

    core::video_format_desc              format_desc_;
    int                                  channel_index_;
    NDIlib_v3*                           ndi_lib_;
    NDIlib_video_frame_v2_t              ndi_video_frame_;
    NDIlib_audio_frame_interleaved_32s_t ndi_audio_frame_;
    std::shared_ptr<uint8_t>             field_data_;
    spl::shared_ptr<diagnostics::graph>  graph_;
    caspar::timer                        tick_timer_;
    caspar::timer                        frame_timer_;
    int                                  frame_no_;
    executor                             executor_;

    std::unique_ptr<NDIlib_send_instance_t, std::function<void(NDIlib_send_instance_t*)>> ndi_send_instance_;

  public:
    newtek_ndi_consumer(std::wstring name, bool allow_fields, NDIlib_FourCC_type_e video_format)
        : name_(!name.empty() ? name : default_ndi_name())
        , instance_no_(instances_++)
        , frame_no_(0)
        , allow_fields_(allow_fields)
        , video_format_(video_format)
        , channel_index_(0)
        , executor_(L"ndi_consumer[" + (!name.empty() ? name : default_ndi_name()) + L"]")
    {
        ndi_lib_ = ndi::load_library();
        graph_->set_text(print());
        graph_->set_color("frame-time", diagnostics::color(0.5f, 1.0f, 0.2f));
        graph_->set_color("tick-time", diagnostics::color(0.0f, 0.6f, 0.9f));
        graph_->set_color("dropped-frame", diagnostics::color(0.3f, 0.6f, 0.3f));
        diagnostics::register_graph(graph_);
        executor_.set_capacity(3);
    }

    ~newtek_ndi_consumer() {}

    // frame_consumer

    void initialize(const core::video_format_desc& format_desc, int channel_index) override
    {
        format_desc_   = format_desc;
        channel_index_ = channel_index;

        NDIlib_send_create_t NDI_send_create_desc;

        auto tmp_name                   = u8(name_);
        NDI_send_create_desc.p_ndi_name = tmp_name.c_str();

        ndi_send_instance_ = {new NDIlib_send_instance_t(ndi_lib_->NDIlib_send_create(&NDI_send_create_desc)),
                              [this](auto p) { this->ndi_lib_->NDIlib_send_destroy(*p); }};

        ndi_video_frame_.xres                 = format_desc.width;
        ndi_video_frame_.yres                 = format_desc.height;
        ndi_video_frame_.frame_rate_N         = format_desc.framerate.numerator();
        ndi_video_frame_.frame_rate_D         = format_desc.framerate.denominator();
        ndi_video_frame_.FourCC               = video_format_;
        if (ndi_video_frame_.FourCC == NDIlib_FourCC_type_UYVA || ndi_video_frame_.FourCC == NDIlib_FourCC_type_UYVY) {
            ndi_video_frame_.line_stride_in_bytes = format_desc.width * 2;
        } else {
            ndi_video_frame_.line_stride_in_bytes = format_desc.width * 4;
        }
        ndi_video_frame_.frame_format_type    = NDIlib_frame_format_type_progressive;

        if (format_desc.field_count == 2 && allow_fields_) {
            ndi_video_frame_.yres /= 2;
            ndi_video_frame_.frame_rate_N /= 2;
            ndi_video_frame_.picture_aspect_ratio = format_desc.width * 1.0f / format_desc.height;
        }

        if ((format_desc.field_count == 2 && allow_fields_) || ndi_video_frame_.FourCC != NDIlib_FourCC_type_BGRA) {
            field_data_.reset(new uint8_t[ndi_video_frame_.line_stride_in_bytes * ndi_video_frame_.yres + (ndi_video_frame_.FourCC == NDIlib_FourCC_type_UYVA ? ndi_video_frame_.xres * ndi_video_frame_.yres : 0)],
                              std::default_delete<uint8_t[]>());
            ndi_video_frame_.p_data = field_data_.get();
        }

        ndi_audio_frame_.sample_rate = format_desc_.audio_sample_rate;
        ndi_audio_frame_.no_channels = format_desc_.audio_channels;
        ndi_audio_frame_.timecode    = NDIlib_send_timecode_synthesize;

        graph_->set_text(print());
        // CASPAR_VERIFY(ndi_send_instance_);
    }

    std::future<bool> send(core::const_frame frame) override
    {
        CASPAR_VERIFY(format_desc_.height * format_desc_.width * 4 == frame.image_data(0).size());

        if (executor_.size() < executor_.capacity()) {
            executor_.begin_invoke([=] {
                graph_->set_value("tick-time", tick_timer_.elapsed() * format_desc_.fps * 0.5);
                tick_timer_.restart();
                frame_timer_.restart();
                auto audio_data             = frame.audio_data();
                int  audio_data_size        = static_cast<int>(audio_data.size());
                ndi_audio_frame_.no_samples = audio_data_size / format_desc_.audio_channels;
                ndi_audio_frame_.p_data     = const_cast<int*>(audio_data.data());
                ndi_lib_->NDIlib_util_send_send_audio_interleaved_32s(*ndi_send_instance_, &ndi_audio_frame_);

                if (ndi_video_frame_.FourCC == NDIlib_FourCC_type_UYVA || ndi_video_frame_.FourCC == NDIlib_FourCC_type_UYVY) {
                    for (int y = 0; y < ndi_video_frame_.yres; y++) {
                        uint8_t *p_uyvy = (uint8_t*)ndi_video_frame_.p_data + y * ndi_video_frame_.line_stride_in_bytes;
                        uint8_t *p_alpha = (uint8_t*)ndi_video_frame_.p_data + ndi_video_frame_.line_stride_in_bytes * ndi_video_frame_.yres + y * ndi_video_frame_.xres;
                        uint32_t offset = y;
                        if (format_desc_.field_count == 2 && allow_fields_) {
                            offset = offset * 2 + frame_no_ % 2;
                        }
                        uint8_t *s = (uint8_t*)frame.image_data(0).data() + offset * 4 * ndi_video_frame_.xres;
                        for (int x = 0; x < ndi_video_frame_.xres/2; x++) {
                            p_uyvy[x*4+0] = (uint8_t)(128 - 0.16874 * s[x*8+2] - 0.33126 * s[x*8+1] + 0.50000 * s[x*8+0]); //Cb0
                            p_uyvy[x*4+1] = (uint8_t)(      0.299   * s[x*8+2] + 0.587   * s[x*8+1] + 0.114   * s[x*8+0]); //Y0
                            p_uyvy[x*4+2] = (uint8_t)(128 + 0.50000 * s[x*8+2] - 0.41869 * s[x*8+1] - 0.08131 * s[x*8+0]); //Cr0
                            p_uyvy[x*4+3] = (uint8_t)(      0.299   * s[x*8+6] + 0.587   * s[x*8+5] + 0.114   * s[x*8+4]); //Y1
                            if (ndi_video_frame_.FourCC == NDIlib_FourCC_type_UYVA) {
                                p_alpha[x*2] = s[x*8+3]; // A0
                                p_alpha[x*2+1] = s[x*8+7]; //A1
                            }
                        }
                    }
                    if (format_desc_.field_count == 2 && allow_fields_) {
                        ndi_video_frame_.frame_format_type =
                            (frame_no_ % 2 ? NDIlib_frame_format_type_field_1 : NDIlib_frame_format_type_field_0);
                    }
                } else {
                    if (format_desc_.field_count == 2 && allow_fields_) {
                        ndi_video_frame_.frame_format_type =
                            (frame_no_ % 2 ? NDIlib_frame_format_type_field_1 : NDIlib_frame_format_type_field_0);
                        for (auto y = 0; y < ndi_video_frame_.yres; ++y) {
                            std::memcpy(reinterpret_cast<char*>(ndi_video_frame_.p_data) + y * format_desc_.width * 4,
                                        frame.image_data(0).data() + (y * 2 + frame_no_ % 2) * format_desc_.width * 4,
                                        format_desc_.width * 4);
                        }
                    } else {
                        ndi_video_frame_.p_data = const_cast<uint8_t*>(frame.image_data(0).begin());
                    }
                }

                ndi_lib_->NDIlib_send_send_video_v2(*ndi_send_instance_, &ndi_video_frame_);
                frame_no_++;
                graph_->set_value("frame-time", frame_timer_.elapsed() * format_desc_.fps * 0.5);
            });
        } else {
            graph_->set_tag(caspar::diagnostics::tag_severity::WARNING, "dropped-frame");
        }

        return make_ready_future(true);
    }

    std::wstring print() const override
    {
        if (channel_index_) {
            return L"ndi_consumer[" + boost::lexical_cast<std::wstring>(channel_index_) + L"|" + name_ + L"]";
        } else {
            return L"[ndi_consumer]";
        }
    }

    std::wstring name() const override { return L"ndi"; }

    std::wstring default_ndi_name() const
    {
        return L"CasparCG" + (instance_no_ ? L" " + boost::lexical_cast<std::wstring>(instance_no_) : L"");
    }

    int index() const override { return 900; }

    bool has_synchronization_clock() const override { return false; }
};

std::atomic<int> newtek_ndi_consumer::instances_(0);

NDIlib_FourCC_type_e parse_video_format(std::wstring format)
{
    if (format.length() != 4) {
        return NDIlib_FourCC_type_BGRA;
    }
    format = boost::to_upper_copy(format);
    if (format == L"UYVA") {
        return NDIlib_FourCC_type_UYVA;
    } else if (format == L"UYVY") {
        return NDIlib_FourCC_type_UYVY;
    }
    return NDIlib_FourCC_type_BGRA;
};

spl::shared_ptr<core::frame_consumer> create_ndi_consumer(const std::vector<std::wstring>&                  params,
                                                          std::vector<spl::shared_ptr<core::video_channel>> channels)
{
    if (params.size() < 1 || !boost::iequals(params.at(0), L"NDI"))
        return core::frame_consumer::empty();
    std::wstring name         = get_param(L"NAME", params, L"");
    bool         allow_fields = contains_param(L"ALLOW_FIELDS", params);
    std::wstring video_format       = get_param(L"PIXEL_FORMAT", params, L"BGRA");
    return spl::make_shared<newtek_ndi_consumer>(name, allow_fields, parse_video_format(video_format));
}

spl::shared_ptr<core::frame_consumer>
create_preconfigured_ndi_consumer(const boost::property_tree::wptree&               ptree,
                                  std::vector<spl::shared_ptr<core::video_channel>> channels)
{
    auto name         = ptree.get(L"name", L"");
    bool allow_fields = ptree.get(L"allow-fields", false);
    std::wstring video_format = ptree.get(L"pixel-format", L"BGRA");
    return spl::make_shared<newtek_ndi_consumer>(name, allow_fields, parse_video_format(video_format));
}

}} // namespace caspar::newtek
