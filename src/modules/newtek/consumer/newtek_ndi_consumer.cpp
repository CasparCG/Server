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

#include <chrono>
#include <core/consumer/channel_info.h>
#include <core/consumer/frame_consumer.h>
#include <core/frame/frame.h>
#include <core/mixer/audio/audio_util.h>
#include <core/video_format.h>

#include <common/assert.h>
#include <common/diagnostics/graph.h>
#include <common/env.h>
#include <common/except.h>
#include <common/future.h>
#include <common/param.h>
#include <common/timer.h>

#include <boost/algorithm/string.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/thread.hpp>
#include <ratio>
#include <thread>

#include <tbb/concurrent_queue.h>

#include "../util/ndi.h"

namespace caspar { namespace newtek {

struct newtek_ndi_consumer : public core::frame_consumer
{
    static std::atomic<int> instances_;
    const int               instance_no_;
    const std::wstring      name_;
    const bool              allow_fields_;
    const std::string       discovery_server_url_;
    const bool              use_advertiser_;
    const bool              allow_monitoring_;
    const int               buffer_size_;

    core::video_format_desc                          format_desc_;
    int                                              channel_index_;
    NDIlib_v6*                                       ndi_lib_;
    NDIlib_video_frame_v2_t                          ndi_video_frame_;
    NDIlib_audio_frame_interleaved_32s_t             ndi_audio_frame_;
    std::shared_ptr<uint8_t>                         field_data_;
    spl::shared_ptr<diagnostics::graph>              graph_;
    caspar::timer                                    tick_timer_;
    caspar::timer                                    frame_timer_;
    caspar::timer                                    ndi_timer_;
    int                                              frame_no_;
    tbb::concurrent_bounded_queue<core::const_frame> frame_buffer_;
    boost::thread                                    send_thread;

    std::unique_ptr<NDIlib_send_instance_t, std::function<void(NDIlib_send_instance_t*)>> ndi_send_instance_;
    std::unique_ptr<NDIlib_send_advertiser_instance_t, std::function<void(NDIlib_send_advertiser_instance_t*)>>
        ndi_advertiser_instance_;

  public:
    newtek_ndi_consumer(std::wstring name,
                        bool         allow_fields,
                        std::string  discovery_server_url = "",
                        bool         use_advertiser       = false,
                        bool         allow_monitoring     = true,
                        int          buffer_size          = 16)
        : name_(!name.empty() ? name : default_ndi_name())
        , instance_no_(instances_++)
        , frame_no_(0)
        , allow_fields_(allow_fields)
        , discovery_server_url_(discovery_server_url)
        , use_advertiser_(use_advertiser)
        , allow_monitoring_(allow_monitoring)
        , buffer_size_(buffer_size > 0 ? buffer_size : 1)
        , channel_index_(0)
    {
        ndi_lib_ = ndi::load_library();
        graph_->set_text(print());
        graph_->set_color("frame-time", diagnostics::color(0.5f, 1.0f, 0.2f));
        graph_->set_color("tick-time", diagnostics::color(0.0f, 0.6f, 0.9f));
        graph_->set_color("buffered-frames", diagnostics::color(0.5f, 0.0f, 0.2f));
        graph_->set_color("dropped-frame", diagnostics::color(0.3f, 0.6f, 0.3f));
        graph_->set_color("ndi-tick", diagnostics::color(1.0f, 1.0f, 0.1f));
        diagnostics::register_graph(graph_);
    }

    ~newtek_ndi_consumer()
    {
        if (send_thread.joinable()) {
            // Drop any queued frames, then push an empty sentinel frame to wake the send
            // thread and signal shutdown. Clearing first bounds shutdown latency to a
            // single frame instead of draining the whole buffer at playback speed.
            frame_buffer_.clear();
            frame_buffer_.push(core::const_frame{});
            send_thread.join();
        }
    }

    // frame_consumer

    void initialize(const core::video_format_desc& format_desc,
                    const core::channel_info&      channel_info,
                    int                            port_index) override
    {
        if (send_thread.joinable())
            CASPAR_THROW_EXCEPTION(invalid_operation() << msg_info("Cannot reinitialize ndi-consumer."));

        format_desc_   = format_desc;
        channel_index_ = channel_info.index;

        // Make sure to stop the advertiser before recreating the sender
        ndi_advertiser_instance_.reset();

        NDIlib_send_create_t NDI_send_create_desc;

        auto tmp_name                   = u8(name_);
        NDI_send_create_desc.p_ndi_name = tmp_name.c_str();
        // NDI defaults to clocking on video, however it's very jittery.
        NDI_send_create_desc.clock_video = false;
        NDI_send_create_desc.clock_audio = false;

        ndi_send_instance_ = {new NDIlib_send_instance_t(ndi_lib_->send_create(&NDI_send_create_desc)),
                              [this](auto p) { this->ndi_lib_->send_destroy(*p); }};

        // Create and configure NDI advertiser if enabled
        if (use_advertiser_) {
            if (!ndi_lib_->send_advertiser_create) {
                CASPAR_LOG(warning)
                    << L"NDI advertiser requested but not supported by this NDI SDK version (requires NDI 5.5+)";
            } else {
                // Use constructor for proper initialization
                NDIlib_send_advertiser_create_t advertiser_create_desc(
                    discovery_server_url_.empty() ? nullptr : discovery_server_url_.c_str());

                auto advertiser_instance = ndi_lib_->send_advertiser_create(&advertiser_create_desc);

                if (!advertiser_instance) {
                    CASPAR_LOG(warning) << L"Failed to create NDI advertiser for sender '" << name_ << L"'"
                                        << (discovery_server_url_.empty()
                                                ? L" (using default discovery)"
                                                : L" with server: " + u16(discovery_server_url_));
                } else {
                    ndi_advertiser_instance_ = {
                        new NDIlib_send_advertiser_instance_t(advertiser_instance), [this](auto p) {
                            if (p && *p && this->ndi_lib_->send_advertiser_del_sender &&
                                this->ndi_lib_->send_advertiser_destroy) {
                                // Remove sender before destroying advertiser
                                this->ndi_lib_->send_advertiser_del_sender(*p, *ndi_send_instance_);
                                this->ndi_lib_->send_advertiser_destroy(*p);
                            }
                        }};

                    bool added = ndi_lib_->send_advertiser_add_sender(
                        *ndi_advertiser_instance_, *ndi_send_instance_, allow_monitoring_);

                    if (added) {
                        CASPAR_LOG(info) << L"NDI sender '" << name_ << L"' registered with discovery server"
                                         << (discovery_server_url_.empty() ? L""
                                                                           : L" at " + u16(discovery_server_url_));
                    } else {
                        CASPAR_LOG(warning) << L"Failed to register NDI sender '" << name_
                                            << L"' with advertiser (sender may already be registered)";
                    }
                }
            }
        }

        ndi_video_frame_.xres                 = format_desc.width;
        ndi_video_frame_.yres                 = format_desc.height;
        ndi_video_frame_.frame_rate_N         = format_desc.framerate.numerator() * format_desc.field_count;
        ndi_video_frame_.frame_rate_D         = format_desc.framerate.denominator();
        ndi_video_frame_.FourCC               = NDIlib_FourCC_type_BGRA;
        ndi_video_frame_.line_stride_in_bytes = format_desc.width * 4;
        ndi_video_frame_.frame_format_type    = NDIlib_frame_format_type_progressive;

        if (format_desc.field_count == 2 && allow_fields_) {
            ndi_video_frame_.yres /= 2;
            ndi_video_frame_.frame_rate_N /= 2;
            ndi_video_frame_.picture_aspect_ratio = format_desc.width * 1.0f / format_desc.height;
            field_data_.reset(new uint8_t[ndi_video_frame_.line_stride_in_bytes * ndi_video_frame_.yres],
                              std::default_delete<uint8_t[]>());
            ndi_video_frame_.p_data = field_data_.get();
        }

        ndi_audio_frame_.sample_rate = format_desc_.audio_sample_rate;
        ndi_audio_frame_.no_channels = format_desc_.audio_channels;
        ndi_audio_frame_.timecode    = NDIlib_send_timecode_synthesize;

        graph_->set_text(print());
        // CASPAR_VERIFY(ndi_send_instance_);

        frame_buffer_.set_capacity(buffer_size_);

        send_thread = boost::thread([this]() {
            set_thread_realtime_priority();
            set_thread_name(L"NDI-SEND: " + name_);
            CASPAR_LOG(info) << L"Starting ndi-send thread for ndi output: " << name_;

            // Use a steady clock to generate a near perfect NDI tick time. The clock is
            // aligned once playout starts so there is no catch-up burst.
            auto frametimeUs = static_cast<int>(1000000 / format_desc_.fps);
            auto time_point  = std::chrono::steady_clock::time_point{};
            bool first       = true;

            while (true) {
                core::const_frame frame;
                frame_buffer_.pop(frame);
                if (!frame) {
                    // An empty sentinel frame signals shutdown.
                    break;
                }

                if (first) {
                    // De-jitter pre-roll: once the first frame has arrived, let the producer
                    // build a small cushion before playout so brief upstream stalls do not
                    // underrun the NDI output. The cushion is half the buffer capacity, so a
                    // tiny buffer keeps latency minimal.
                    const int prebuffer = buffer_size_ / 2;
                    std::this_thread::sleep_for(
                        std::chrono::microseconds(static_cast<int64_t>(frametimeUs) * prebuffer));
                    time_point = std::chrono::steady_clock::now() + std::chrono::microseconds(frametimeUs);
                    first      = false;
                }

                graph_->set_value("buffered-frames", static_cast<double>(frame_buffer_.size() + 0.001) / buffer_size_);
                graph_->set_value("ndi-tick", ndi_timer_.elapsed() * format_desc_.fps * 0.5);
                ndi_timer_.restart();
                frame_timer_.restart();

                auto audio_data             = frame.audio_data();
                int  audio_data_size        = static_cast<int>(audio_data.size());
                ndi_audio_frame_.no_samples = audio_data_size / format_desc_.audio_channels;
                ndi_audio_frame_.p_data     = const_cast<int*>(audio_data.data());
                ndi_lib_->util_send_send_audio_interleaved_32s(*ndi_send_instance_, &ndi_audio_frame_);

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

                ndi_lib_->send_send_video_v2(*ndi_send_instance_, &ndi_video_frame_);
                frame_no_++;

                graph_->set_value("frame-time", frame_timer_.elapsed() * format_desc_.fps * 0.5);
                std::this_thread::sleep_until(time_point);
                time_point += std::chrono::microseconds(frametimeUs);
            }
        });
    }

    std::future<bool> send(core::video_field field, core::const_frame frame) override
    {
        graph_->set_value("tick-time", tick_timer_.elapsed() * format_desc_.fps * 0.5);
        tick_timer_.restart();

        // The producer enforces the buffer bound: if the send thread cannot keep up the
        // oldest-rejected frame is dropped here, so memory never grows unbounded.
        if (!frame_buffer_.try_push(std::move(frame))) {
            graph_->set_tag(diagnostics::tag_severity::WARNING, "dropped-frame");
        }

        graph_->set_value("buffered-frames", static_cast<double>(frame_buffer_.size() + 0.001) / buffer_size_);

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

    core::monitor::state state() const override
    {
        core::monitor::state state;
        state["ndi/name"]                 = name_;
        state["ndi/allow_fields"]         = allow_fields_;
        state["ndi/use_advertiser"]       = use_advertiser_;
        state["ndi/allow_monitoring"]     = allow_monitoring_;
        state["ndi/discovery_server_url"] = discovery_server_url_;
        state["ndi/buffer_size"]          = buffer_size_;
        return state;
    }
};

std::atomic<int> newtek_ndi_consumer::instances_(0);

spl::shared_ptr<core::frame_consumer>
create_ndi_consumer(const std::vector<std::wstring>&                         params,
                    const core::video_format_repository&                     format_repository,
                    const std::vector<spl::shared_ptr<core::video_channel>>& channels,
                    const core::channel_info&                                channel_info)
{
    if (params.size() < 1 || !boost::iequals(params.at(0), L"NDI"))
        return core::frame_consumer::empty();

    if (channel_info.depth != common::bit_depth::bit8)
        CASPAR_THROW_EXCEPTION(caspar_exception() << msg_info("Newtek NDI consumer only supports 8-bit color depth."));

    std::wstring name                   = get_param(L"NAME", params, L"");
    bool         allow_fields           = contains_param(L"ALLOW_FIELDS", params);
    bool         use_advertiser         = contains_param(L"USE_ADVERTISER", params);
    bool         allow_monitoring       = get_param(L"ALLOW_MONITORING", params, true);
    int          buffer_size            = get_param(L"BUFFER_SIZE", params, 16);
    std::wstring discovery_server_url_w = get_param(L"DISCOVERY_SERVER", params, L"");
    if (discovery_server_url_w.empty())
        discovery_server_url_w = env::properties().get(L"configuration.ndi.discovery-server", L"");
    std::string discovery_server_url = ndi::apply_default_discovery_port(u8(discovery_server_url_w));

    return spl::make_shared<newtek_ndi_consumer>(
        name, allow_fields, discovery_server_url, use_advertiser, allow_monitoring, buffer_size);
}

spl::shared_ptr<core::frame_consumer>
create_preconfigured_ndi_consumer(const boost::property_tree::wptree&                      ptree,
                                  const core::video_format_repository&                     format_repository,
                                  const std::vector<spl::shared_ptr<core::video_channel>>& channels,
                                  const core::channel_info&                                channel_info)
{
    auto         name                   = ptree.get(L"name", L"");
    bool         allow_fields           = ptree.get(L"allow-fields", false);
    bool         use_advertiser         = ptree.get(L"use-advertiser", false);
    bool         allow_monitoring       = ptree.get(L"allow-monitoring", true);
    int          buffer_size            = ptree.get(L"buffer-size", 16);
    std::wstring discovery_server_url_w = ptree.get(L"discovery-server", L"");
    if (discovery_server_url_w.empty())
        discovery_server_url_w = env::properties().get(L"configuration.ndi.discovery-server", L"");
    std::string discovery_server_url = ndi::apply_default_discovery_port(u8(discovery_server_url_w));

    if (channel_info.depth != common::bit_depth::bit8)
        CASPAR_THROW_EXCEPTION(caspar_exception() << msg_info("Newtek NDI consumer only supports 8-bit color depth."));

    return spl::make_shared<newtek_ndi_consumer>(
        name, allow_fields, discovery_server_url, use_advertiser, allow_monitoring, buffer_size);
}

}} // namespace caspar::newtek
