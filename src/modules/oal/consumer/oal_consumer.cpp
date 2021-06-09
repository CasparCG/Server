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

#include "oal_consumer.h"

#include <common/diagnostics/graph.h>
#include <common/env.h>
#include <common/except.h>
#include <common/executor.h>
#include <common/future.h>
#include <common/log.h>
#include <common/param.h>
#include <common/timer.h>

#include <core/consumer/frame_consumer.h>
#include <core/frame/frame.h>
#include <core/mixer/audio/audio_mixer.h>
#include <core/video_format.h>

#include <boost/property_tree/ptree.hpp>

#include <tbb/concurrent_queue.h>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4244)
#endif
extern "C" {
#define __STDC_CONSTANT_MACROS
#define __STDC_LIMIT_MACROS
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <memory>
#include <vector>

#include <AL/al.h>
#include <AL/alc.h>

namespace caspar { namespace oal {

class device
{
    ALCdevice*  device_  = nullptr;
    ALCcontext* context_ = nullptr;

  public:
    device()
    {
        device_ = alcOpenDevice(nullptr);

        if (device_ == nullptr)
            CASPAR_THROW_EXCEPTION(invalid_operation() << msg_info("Failed to initialize audio device."));

        context_ = alcCreateContext(device_, nullptr);

        if (context_ == nullptr)
            CASPAR_THROW_EXCEPTION(invalid_operation() << msg_info("Failed to create audio context."));

        if (alcMakeContextCurrent(context_) == ALC_FALSE)
            CASPAR_THROW_EXCEPTION(invalid_operation() << msg_info("Failed to activate audio context."));
    }

    ~device()
    {
        alcMakeContextCurrent(nullptr);

        if (context_ != nullptr)
            alcDestroyContext(context_);

        if (device_ != nullptr)
            alcCloseDevice(device_);
    }

    ALCdevice* get() { return device_; }
};

void init_device()
{
    static std::unique_ptr<device> instance;
    static std::once_flag          f;

    std::call_once(f, [] { instance = std::make_unique<device>(); });
}

struct oal_consumer : public core::frame_consumer
{
    spl::shared_ptr<diagnostics::graph> graph_;
    caspar::timer                       perf_timer_;
    int                                 channel_index_ = -1;

    core::video_format_desc format_desc_;

    ALuint              source_ = 0;
    std::vector<ALuint> buffers_;
    bool                started_  = false;
    int                 duration_ = 1920;

    std::shared_ptr<SwrContext> swr_;

    executor executor_{L"oal_consumer"};

  public:
    oal_consumer()
    {
        init_device();

        graph_->set_color("tick-time", diagnostics::color(0.0f, 0.6f, 0.9f));
        graph_->set_color("dropped-frame", diagnostics::color(0.3f, 0.6f, 0.3f));
        graph_->set_color("late-frame", diagnostics::color(0.6f, 0.3f, 0.3f));
        diagnostics::register_graph(graph_);
    }

    ~oal_consumer() override
    {
        executor_.invoke([=] {
            if (source_ != 0u) {
                alSourceStop(source_);
                alDeleteSources(1, &source_);
            }

            for (auto& buffer : buffers_) {
                if (buffer != 0u)
                    alDeleteBuffers(1, &buffer);
            }
        });
    }

    // frame consumer

    void initialize(const core::video_format_desc& format_desc, int channel_index) override
    {
        format_desc_   = format_desc;
        channel_index_ = channel_index;
        graph_->set_text(print());

        executor_.begin_invoke([=] {
            duration_ = *std::min_element(format_desc_.audio_cadence.begin(), format_desc_.audio_cadence.end());
            buffers_.resize(8);
            alGenBuffers(static_cast<ALsizei>(buffers_.size()), buffers_.data());
            alGenSources(1, &source_);

            alSourcei(source_, AL_LOOPING, AL_FALSE);
        });
    }

    std::future<bool> send(core::const_frame frame) override
    {
        executor_.begin_invoke([=] {
            auto dst            = std::shared_ptr<AVFrame>(av_frame_alloc(), [](AVFrame* ptr) { av_frame_free(&ptr); });
            dst->format         = AV_SAMPLE_FMT_S16;
            dst->sample_rate    = format_desc_.audio_sample_rate;
            dst->channels       = 2;
            dst->channel_layout = av_get_default_channel_layout(dst->channels);
            dst->nb_samples     = duration_;
            if (av_frame_get_buffer(dst.get(), 32) < 0) {
                // TODO FF error
                CASPAR_THROW_EXCEPTION(invalid_argument());
            }
            std::memset(dst->extended_data[0], 0, dst->linesize[0]);

            if (!started_) {
                for (ALuint& buffer : buffers_) {
                    alBufferData(buffer,
                                 AL_FORMAT_STEREO16,
                                 dst->extended_data[0],
                                 static_cast<ALsizei>(dst->linesize[0]),
                                 dst->sample_rate);
                    alSourceQueueBuffers(source_, 1, &buffer);
                }

                alSourcePlay(source_);
                started_ = true;

                return;
            }

            auto src            = std::shared_ptr<AVFrame>(av_frame_alloc(), [](AVFrame* ptr) { av_frame_free(&ptr); });
            src->format         = AV_SAMPLE_FMT_S32;
            src->sample_rate    = format_desc_.audio_sample_rate;
            src->channels       = format_desc_.audio_channels;
            src->channel_layout = av_get_default_channel_layout(src->channels);
            src->nb_samples     = static_cast<int>(frame.audio_data().size() / src->channels);
            src->extended_data[0] = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(frame.audio_data().data()));
            src->linesize[0]      = static_cast<int>(frame.audio_data().size() * sizeof(int32_t));

            if (!swr_) {
                swr_.reset(swr_alloc(), [](SwrContext* ptr) { swr_free(&ptr); });
                if (!swr_) {
                    CASPAR_THROW_EXCEPTION(bad_alloc());
                }
                if (swr_config_frame(swr_.get(), dst.get(), src.get()) < 0) {
                    // TODO FF error
                    CASPAR_THROW_EXCEPTION(invalid_argument());
                }
                if (swr_init(swr_.get()) < 0) {
                    // TODO FF error
                    CASPAR_THROW_EXCEPTION(invalid_argument());
                }
            }

            if (swr_convert_frame(swr_.get(), nullptr, src.get()) < 0) {
                // TODO FF error
                CASPAR_THROW_EXCEPTION(invalid_argument());
            }

            ALint processed = 0;
            alGetSourceiv(source_, AL_BUFFERS_PROCESSED, &processed);

            auto in_duration  = swr_get_delay(swr_.get(), 48000);
            auto out_duration = static_cast<int64_t>(processed) * duration_;
            auto delta        = static_cast<int>(out_duration - in_duration);

            ALenum state;
            alGetSourcei(source_, AL_SOURCE_STATE, &state);
            if (state != AL_PLAYING) {
                while (delta > duration_) {
                    ALuint buffer = 0;
                    alSourceUnqueueBuffers(source_, 1, &buffer);
                    if (buffer == 0u) {
                        break;
                    }
                    alBufferData(buffer,
                                 AL_FORMAT_STEREO16,
                                 dst->extended_data[0],
                                 static_cast<ALsizei>(dst->linesize[0]),
                                 dst->sample_rate);
                    alSourceQueueBuffers(source_, 1, &buffer);
                    delta -= duration_;
                }

                alSourcePlay(source_);
            }

            // TODO (fix)
            // if (std::abs(delta) > duration_ && swr_set_compensation(swr_.get(), delta, 2000) < 0) {
            //    // TODO FF error
            //    CASPAR_THROW_EXCEPTION(invalid_argument());
            //}

            for (auto n = 0; n < processed; ++n) {
                if (swr_get_delay(swr_.get(), 48000) < dst->nb_samples) {
                    break;
                }

                ALuint buffer = 0;
                alSourceUnqueueBuffers(source_, 1, &buffer);
                if (buffer == 0u) {
                    // TODO error
                    break;
                }

                if (swr_convert_frame(swr_.get(), dst.get(), nullptr) != 0) {
                    // TODO FF error
                    CASPAR_THROW_EXCEPTION(invalid_argument());
                }

                alBufferData(buffer,
                             AL_FORMAT_STEREO16,
                             dst->extended_data[0],
                             static_cast<ALsizei>(dst->linesize[0]),
                             dst->sample_rate);
                alSourceQueueBuffers(source_, 1, &buffer);
            }

            graph_->set_value("tick-time", perf_timer_.elapsed() * format_desc_.fps * 0.5);
            perf_timer_.restart();
        });

        return make_ready_future(true);
    }

    std::wstring print() const override
    {
        return L"oal[" + std::to_wstring(channel_index_) + L"|" + format_desc_.name + L"]";
    }

    std::wstring name() const override { return L"system-audio"; }

    bool has_synchronization_clock() const override { return false; }

    int index() const override { return 500; }
};

spl::shared_ptr<core::frame_consumer> create_consumer(const std::vector<std::wstring>&                         params,
                                                      const std::vector<spl::shared_ptr<core::video_channel>>& channels)
{
    if (params.empty() || !boost::iequals(params.at(0), L"AUDIO"))
        return core::frame_consumer::empty();

    return spl::make_shared<oal_consumer>();
}

spl::shared_ptr<core::frame_consumer>
create_preconfigured_consumer(const boost::property_tree::wptree&                      ptree,
                              const std::vector<spl::shared_ptr<core::video_channel>>& channels)
{
    return spl::make_shared<oal_consumer>();
}

}} // namespace caspar::oal
