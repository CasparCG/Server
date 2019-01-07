/*
 * Copyright 2013 NewTek
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
 * Author: Robert Nagy, ronag@live.com
 */

#include "../StdAfx.h"

#include "newtek_ivga_consumer.h"

#include <core/consumer/frame_consumer.h>
#include <core/frame/frame.h>
#include <core/mixer/audio/audio_util.h>
#include <core/video_format.h>

#include <common/assert.h>
#include <common/diagnostics/graph.h>
#include <common/future.h>
#include <common/param.h>
#include <common/timer.h>

#include <boost/algorithm/string.hpp>
#include <boost/property_tree/ptree.hpp>

#include <atomic>

#include "../util/air_send.h"

namespace caspar { namespace newtek {

struct newtek_ivga_consumer : public core::frame_consumer
{
    core::video_format_desc             format_desc_;
    std::shared_ptr<void>               air_send_;
    std::atomic<bool>                   connected_ = false;
    spl::shared_ptr<diagnostics::graph> graph_;
    timer                               tick_timer_;

  public:
    newtek_ivga_consumer()
    {
        if (!airsend::is_available()) {
            CASPAR_THROW_EXCEPTION(not_supported() << msg_info(airsend::dll_name() + L" not available"));
        }

        graph_->set_text(print());
        graph_->set_color("frame-time", diagnostics::color(0.5f, 1.0f, 0.2f));
        graph_->set_color("tick-time", diagnostics::color(0.0f, 0.6f, 0.9f));
        graph_->set_color("dropped-frame", diagnostics::color(0.3f, 0.6f, 0.3f));
        diagnostics::register_graph(graph_);
    }

    ~newtek_ivga_consumer() {}

    // frame_consumer

    void initialize(const core::video_format_desc& format_desc, int channel_index) override
    {
        format_desc_ = format_desc;

        air_send_.reset(airsend::create(format_desc.width,
                                        format_desc.height,
                                        format_desc.time_scale,
                                        format_desc.duration,
                                        true,
                                        static_cast<float>(format_desc.square_width) /
                                            static_cast<float>(format_desc.square_height),
                                        true,
                                        format_desc.audio_channels,
                                        format_desc.audio_sample_rate),
                        airsend::destroy);

        CASPAR_VERIFY(air_send_);
    }

    std::future<bool> send(core::frame_timecode timecode, core::const_frame frame) override
    {
        CASPAR_VERIFY(format_desc_.height * format_desc_.width * 4 == frame.image_data(0).size());

        graph_->set_value("tick-time", tick_timer_.elapsed() * format_desc_.fps * 0.5);
        tick_timer_.restart();

        caspar::timer frame_timer;

        {
            auto audio_buffer = core::audio_32_to_16(frame.audio_data());
            airsend::add_audio(air_send_.get(),
                               audio_buffer.data(),
                               static_cast<int>(audio_buffer.size()) / format_desc_.audio_channels);
        }

        {
            connected_ = airsend::add_frame_bgra(air_send_.get(), frame.image_data(0).begin());
        }

        graph_->set_text(print());
        graph_->set_value("frame-time", frame_timer.elapsed() * format_desc_.fps * 0.5);

        return make_ready_future(true);
    }

    std::wstring print() const override
    {
        return connected_ ? L"newtek-ivga[connected]" : L"newtek-ivga[not connected]";
    }

    std::wstring name() const override { return L"newtek-ivga"; }

    int index() const override { return 900; }

    bool has_synchronization_clock() const override { return false; }
};

spl::shared_ptr<core::frame_consumer> create_ivga_consumer(const std::vector<std::wstring>&                  params,
                                                           std::vector<spl::shared_ptr<core::video_channel>> channels)
{
    if (params.size() < 1 || !boost::iequals(params.at(0), L"NEWTEK_IVGA"))
        return core::frame_consumer::empty();

    return spl::make_shared<newtek_ivga_consumer>();
}

spl::shared_ptr<core::frame_consumer>
create_preconfigured_ivga_consumer(const boost::property_tree::wptree&               ptree,
                                   std::vector<spl::shared_ptr<core::video_channel>> channels)
{
    return spl::make_shared<newtek_ivga_consumer>();
}

}} // namespace caspar::newtek
