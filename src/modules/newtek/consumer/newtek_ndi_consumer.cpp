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

#include <boost/thread/exceptions.hpp>
#include <chrono>
#include <condition_variable>
#include <core/consumer/channel_info.h>
#include <core/consumer/frame_consumer.h>
#include <core/frame/frame.h>
#include <core/mixer/audio/audio_util.h>
#include <core/video_format.h>

#include <common/assert.h>
#include <common/diagnostics/graph.h>
#include <common/env.h>
#include <common/except.h>
#include <common/executor.h>
#include <common/future.h>
#include <common/param.h>
#include <common/timer.h>

#include <boost/algorithm/string.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/thread.hpp>
#include <ratio>
#include <thread>

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

    core::video_format_desc              format_desc_;
    int                                  channel_index_;
    NDIlib_v6*                           ndi_lib_;
    NDIlib_video_frame_v2_t              ndi_video_frame_;
    NDIlib_audio_frame_interleaved_32s_t ndi_audio_frame_;
    std::shared_ptr<uint8_t>             field_data_;
    spl::shared_ptr<diagnostics::graph>  graph_;
    caspar::timer                        tick_timer_;
    caspar::timer                        frame_timer_;
    caspar::timer                        ndi_timer_;
    int                                  frame_no_;
    std::mutex                           buffer_mutex_;
    std::condition_variable              buffer_cond_;
    std::condition_variable              worker_cond_;
    bool                                 ready_for_frame_;
    std::queue<core::const_frame>        buffer_;
    boost::thread                        send_thread;
    executor                             executor_;

    std::unique_ptr<NDIlib_send_instance_t, std::function<void(NDIlib_send_instance_t*)>> ndi_send_instance_;
    std::unique_ptr<NDIlib_send_advertiser_instance_t, std::function<void(NDIlib_send_advertiser_instance_t*)>>
        ndi_advertiser_instance_;

  public:
    newtek_ndi_consumer(std::wstring name,
                        bool         allow_fields,
                        std::string  discovery_server_url = "",
                        bool         use_advertiser       = false,
                        bool         allow_monitoring     = true)
        : name_(!name.empty() ? name : default_ndi_name())
        , instance_no_(instances_++)
        , frame_no_(0)
        , allow_fields_(allow_fields)
        , discovery_server_url_(discovery_server_url)
        , use_advertiser_(use_advertiser)
        , allow_monitoring_(allow_monitoring)
        , channel_index_(0)
        , executor_(L"ndi_consumer[" + std::to_wstring(instance_no_) + L"]")
    {
        ndi_lib_ = ndi::load_library();
        graph_->set_text(print());
        graph_->set_color("frame-time", diagnostics::color(0.5f, 1.0f, 0.2f));
        graph_->set_color("tick-time", diagnostics::color(0.0f, 0.6f, 0.9f));
        graph_->set_color("buffered-frames", diagnostics::color(0.5f, 0.0f, 0.2f));
        graph_->set_color("ndi-tick", diagnostics::color(1.0f, 1.0f, 0.1f));
        diagnostics::register_graph(graph_);
    }

    ~newtek_ndi_consumer()
    {
        if (send_thread.joinable()) {
            send_thread.interrupt();
            send_thread.join();
        }
    }

    // frame_consumer

    void initialize(const core::video_format_desc& format_desc,
                    const core::channel_info&      channel_info,
                    int                            port_index) override
    {
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

        send_thread = boost::thread([this]() {
            set_thread_realtime_priority();
            set_thread_name(L"NDI-SEND: " + name_);
            CASPAR_LOG(info) << L"Starting ndi-send thread for ndi output: " << name_;
            try {
                auto buffer_size = buffer_.size();
                // Buffer a few frames to keep NDI going when caspar is slow on a few frames
                // This can be removed when CasparCG doesn't periodally slows down on frames
                while (!send_thread.interruption_requested()) {
                    {
                        std::unique_lock<std::mutex> lock(buffer_mutex_);
                        worker_cond_.wait(lock, [&] { return buffer_.size() > buffer_size; });
                        graph_->set_value("buffered-frames", static_cast<double>(buffer_.size() + 0.001) / 8);
                        buffer_size = buffer_.size();
                        if (buffer_.size() >= 8) {
                            break;
                        }
                    }
                }

                // Use steady clock to generate a near perfect NDI tick time.
                auto frametimeUs = static_cast<int>(1000000 / format_desc_.fps);
                auto time_point  = std::chrono::steady_clock::now();
                time_point += std::chrono::microseconds(frametimeUs);
                while (!send_thread.interruption_requested()) {
                    core::const_frame frame;
                    {
                        std::unique_lock<std::mutex> lock(buffer_mutex_);
                        worker_cond_.wait(lock, [&] { return !buffer_.empty(); });
                        graph_->set_value("buffered-frames", static_cast<double>(buffer_.size() + 0.001) / 8);
                        frame = std::move(buffer_.front());
                        buffer_.pop();
                    }
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
            } catch (boost::thread_interrupted) {
                // NOTHING
            }
        });
    }

    std::future<bool> send(core::video_field field, core::const_frame frame) override
    {
        return executor_.begin_invoke([=, this] {
            graph_->set_value("tick-time", tick_timer_.elapsed() * format_desc_.fps * 0.5);
            tick_timer_.restart();
            {
                std::unique_lock<std::mutex> lock(buffer_mutex_);
                buffer_.push(std::move(frame));
            }
            worker_cond_.notify_all();
            return true;
        });
    }

    std::wstring print() const override
    {
        auto id_str = consumer_id().empty() ? L"" : L"|id=" + consumer_id();
        if (channel_index_) {
            return L"ndi_consumer[" + boost::lexical_cast<std::wstring>(channel_index_) + L"|" + name_ + id_str + L"]";
        } else {
            return L"[ndi_consumer" + id_str + L"]";
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
        if (!consumer_id().empty())
            state["ndi/consumer_id"] = consumer_id();
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
    std::wstring discovery_server_url_w = get_param(L"DISCOVERY_SERVER", params, L"");
    if (discovery_server_url_w.empty())
        discovery_server_url_w = env::properties().get(L"configuration.ndi.discovery-server", L"");
    std::string discovery_server_url = ndi::apply_default_discovery_port(u8(discovery_server_url_w));

    return spl::make_shared<newtek_ndi_consumer>(
        name, allow_fields, discovery_server_url, use_advertiser, allow_monitoring);
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
    std::wstring discovery_server_url_w = ptree.get(L"discovery-server", L"");
    if (discovery_server_url_w.empty())
        discovery_server_url_w = env::properties().get(L"configuration.ndi.discovery-server", L"");
    std::string discovery_server_url = ndi::apply_default_discovery_port(u8(discovery_server_url_w));

    if (channel_info.depth != common::bit_depth::bit8)
        CASPAR_THROW_EXCEPTION(caspar_exception() << msg_info("Newtek NDI consumer only supports 8-bit color depth."));

    return spl::make_shared<newtek_ndi_consumer>(
        name, allow_fields, discovery_server_url, use_advertiser, allow_monitoring);
}

}} // namespace caspar::newtek
