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
#include <common/timespan.h>
#include <common/utf.h>

#include <core/consumer/channel_info.h>
#include <core/consumer/frame_consumer.h>
#include <core/frame/frame.h>
#include <core/video_format.h>

#include <boost/property_tree/ptree.hpp>

#include <tbb/concurrent_queue.h>

#pragma warning(disable : 4267)

extern "C" {
#define __STDC_CONSTANT_MACROS
#define __STDC_LIMIT_MACROS
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <ranges>
#include <string>
#include <thread>
#include <vector>

#include <AL/al.h>
#include <AL/alc.h>
#define AL_ALEXT_PROTOTYPES
#include <AL/alext.h>

namespace caspar::oal {

using namespace std::chrono_literals;

class device
{
    struct device_deleter { void operator()(ALCdevice* p) { alcCloseDevice(p); } };
    std::unique_ptr<ALCdevice, device_deleter> device_;

    struct context_deleter { void operator()(ALCcontext* p) { alcDestroyContext(p); } };
    std::unique_ptr<ALCcontext, context_deleter> context_;

    explicit device(std::string device_name)
    {
        int def_device = 0, enum_devices = 0;
        if (alcIsExtensionPresent(nullptr, "ALC_ENUMERATE_ALL_EXT")) {
            def_device = ALC_DEFAULT_ALL_DEVICES_SPECIFIER;
            enum_devices = ALC_ALL_DEVICES_SPECIFIER;
        } else if (alcIsExtensionPresent(nullptr, "ALC_ENUMERATION_EXT")) {
            def_device = ALC_DEFAULT_DEVICE_SPECIFIER;
            enum_devices = ALC_DEVICE_SPECIFIER;
        }

        if (device_name.empty()) {
            if (!def_device)
                CASPAR_THROW_EXCEPTION(not_supported() << msg_info("OpenAL device enumeration not supported"));

            auto name = alcGetString(NULL, def_device);
            if (!name || !*name)
                CASPAR_THROW_EXCEPTION(operation_failed() << msg_info("Default OpenAL device not found"));

            device_name = name;
            CASPAR_LOG(info) << "Using default OpenAL device: " << device_name;
        }
        else if (enum_devices) {
            auto clean = [](const std::string& s) {
                auto v = s | std::views::filter(::isgraph) | std::views::transform(::tolower);
                return std::string{v.begin(), v.end()};
            };

            std::map<std::string, std::string> device_names;

            if (auto names = alcGetString(nullptr, enum_devices)) {
                while (*names) {
                    std::string name = names;
                    device_names[clean(name)] = name;

                    names += name.size() + 1;
                }
            }

            auto it = device_names.find(clean(device_name));
            if (it == device_names.end()) {
                CASPAR_LOG(info) << "-------- OpenAL Devices -------";
                for (auto&& [_, name] : device_names) CASPAR_LOG(info) << name;
                CASPAR_LOG(info) << "-------- OpenAL Devices -------";

                CASPAR_THROW_EXCEPTION(invalid_operation() << msg_info("Invalid OpenAL device: " + device_name));
            }
            else {
                device_name = it->second;
                CASPAR_LOG(info) << "Using OpenAL device: " << device_name;
            }
        }

        device_.reset(alcOpenDevice(device_name.data()));
        if (!device_) CASPAR_THROW_EXCEPTION(invalid_operation() << msg_info("Failed to initialize device."));

        context_.reset(alcCreateContext(device_.get(), nullptr));
        if (!context_) CASPAR_THROW_EXCEPTION(invalid_operation() << msg_info("Failed to create context."));

        if (!alcIsExtensionPresent(device_.get(), "ALC_EXT_thread_local_context"))
            CASPAR_THROW_EXCEPTION(not_supported() << msg_info("Device does not support ALC_EXT_thread_local_context"));
    }

    inline static std::mutex mutex_;
    inline static std::map<std::string, std::weak_ptr<device>> devices_;

public:
    static std::shared_ptr<device> open(const std::string& device_name)
    {
        std::lock_guard guard{mutex_};

        auto& weak = devices_[device_name];
        auto shared = weak.lock();
        if (!shared) weak = shared = std::shared_ptr<device>{new device{device_name}};

        return shared;
    }

    void activate_context()
    {
        if (context_.get() != alcGetThreadContext())
            alcSetThreadContext(context_.get());
    }

    void deactivate_context()
    {
        if (context_.get() == alcGetThreadContext())
            alcSetThreadContext(nullptr);
    }
};

struct oal_consumer : public core::frame_consumer
{
    spl::shared_ptr<diagnostics::graph> graph_;
    caspar::timer                       tick_timer_, frame_timer_;
    int                                 channel_index_ = -1;

    core::video_format_desc format_desc_;

    std::shared_ptr<device> device_;
    ALuint              source_ = 0;
    std::vector<ALuint> buffers_;
    std::queue<ALuint>  free_;
    timespan            delay_;

    std::mutex          mutex_;
    std::condition_variable free_cv_;
    std::atomic<bool>   stop_{false};

    struct swr_deleter { void operator()(SwrContext* p) { swr_free(&p); } };
    std::unique_ptr<SwrContext, swr_deleter> swr_;

    executor executor_{L"oal_consumer"};

  public:
    explicit oal_consumer(const std::string& device_name, const timespan& delay) :
        device_{ device::open(device_name) }, delay_{delay}
    {
        using diagnostics::color;
        graph_->set_color("tick-time",  color(0.0, 0.6, 0.9));
        graph_->set_color("queue-size", color(0.2, 0.9, 0.9));
        graph_->set_color("frame-time", color(0.1, 1.0, 0.1));
        graph_->set_color("drop-out",   color(0.6, 0.3, 0.3));
        diagnostics::register_graph(graph_);
    }

    ~oal_consumer() override
    {
        stop_ = true;
        free_cv_.notify_all();

        executor_.invoke([this] {
            if (source_) {
                std::lock_guard guard{mutex_};
                device_->activate_context();

                alSourceStop(source_);
                alDeleteSources(1, &source_);
                alDeleteBuffers(buffers_.size(), buffers_.data());

                device_->deactivate_context();
            }
        });
    }

    // frame consumer

    void initialize(const core::video_format_desc& format_desc,
                    const core::channel_info&      channel_info,
                    int                            port_index) override
    {
        format_desc_   = format_desc;
        channel_index_ = channel_info.index;
        graph_->set_text(print());

        executor_.begin_invoke([this] {
            AVChannelLayout from, to;
            av_channel_layout_default(&from, format_desc_.audio_channels);
            av_channel_layout_default(&to, 2); // stereo

            SwrContext* raw = nullptr;
            swr_alloc_set_opts2(&raw,
                &to,   AV_SAMPLE_FMT_S16, format_desc_.audio_sample_rate,
                &from, AV_SAMPLE_FMT_S32, format_desc_.audio_sample_rate,
                0, nullptr
            );
            swr_.reset(raw);
            swr_init(raw);

            auto num_silence = delay_.in_frames(format_desc_.fps);
            num_silence = std::clamp<int>(num_silence, 1, format_desc_.fps);

            CASPAR_LOG(info) << print() << " Latency: " << num_silence << " frames";

            std::lock_guard guard{mutex_};
            device_->activate_context();

            buffers_.resize(num_silence + 2);
            alGenBuffers(buffers_.size(), buffers_.data());

            alGenSources(1, &source_);

            std::vector<std::int16_t> silence;
            for (auto n = 0; n < buffers_.size(); ++n) {
                auto buf = buffers_[n];

                if (n < num_silence) {
                    auto cadence = format_desc_.audio_cadence[n % format_desc_.audio_cadence.size()];
                    silence.resize(cadence * 2); // stereo

                    alBufferData(buf, AL_FORMAT_STEREO16, silence.data(),
                        silence.size() * sizeof(std::int16_t),
                        format_desc_.audio_sample_rate
                    );
                    alSourceQueueBuffers(source_, 1, &buf);
                } else {
                    free_.push(buf);
                }
            }
        });
    }

    std::future<bool> send(core::video_field field, core::const_frame frame) override
    {
        executor_.begin_invoke([this, frame = std::move(frame)] {
            // resample and downmix
            auto&& audio_in = frame.audio_data();
            int num_samples = audio_in.size() / format_desc_.audio_channels;

            std::vector<std::int16_t> audio_out(num_samples * 2); // stereo

            auto from = reinterpret_cast<const std::uint8_t*>(audio_in.data());
            auto to = reinterpret_cast<std::uint8_t*>(audio_out.data());
            swr_convert(swr_.get(), &to, num_samples, &from, num_samples);

            double free_size;
            bool dropout = false;
            {
                // submit to OAL queue
                std::unique_lock lock{mutex_};
                free_cv_.wait(lock, [this] { return !free_.empty() || stop_; });
                device_->activate_context();

                auto buf = free_.front();
                free_.pop();
                alBufferData(buf, AL_FORMAT_STEREO16, audio_out.data(),
                    audio_out.size() * sizeof(std::int16_t),
                    format_desc_.audio_sample_rate
                );
                alSourceQueueBuffers(source_, 1, &buf);
                free_size = free_.size();

                ALenum state = 0;
                alGetSourcei(source_, AL_SOURCE_STATE, &state);
                if (state != AL_PLAYING) {
                    dropout = true;
                    alSourcePlay(source_);
                }
            }

            graph_->set_value("tick-time", tick_timer_.elapsed() * format_desc_.fps * 0.5);
            tick_timer_.restart();
            graph_->set_value("queue-size", 1 - free_size / buffers_.size());
            if (dropout) graph_->set_tag(diagnostics::tag_severity::WARNING, "drop-out");
        });

        return std::async(std::launch::deferred, [this] {
            // wait until one buffer is done playing
            while (!stop_) {
                ALint done = 0;
                {
                    std::lock_guard guard{mutex_};
                    device_->activate_context();

                    alGetSourcei(source_, AL_BUFFERS_PROCESSED, &done);
                    if (done) {
                        ALuint buf;
                        alSourceUnqueueBuffers(source_, 1, &buf);
                        free_.push(buf);
                    }
                }
                if (done) {
                    free_cv_.notify_one();
                    graph_->set_value("frame-time", frame_timer_.elapsed() * format_desc_.fps * 0.5);
                    frame_timer_.restart();
                    return true;
                }
                std::this_thread::sleep_for(100us);
            }
            return false;
        });
    }

    std::wstring print() const override
    {
        auto id_str = consumer_id().empty() ? L"" : L"|id=" + consumer_id();
        return L"oal[" + std::to_wstring(channel_index_) + L"|" + format_desc_.name + id_str + L"]";
    }

    std::wstring name() const override { return L"system-audio"; }

    bool has_synchronization_clock() const override { return true; }

    int index() const override { return 500; }

    core::monitor::state state() const override
    {
        core::monitor::state state;
        if (!consumer_id().empty())
            state["system-audio/consumer_id"] = consumer_id();
        return state;
    }
};

static constexpr auto old_config_path = L"configuration.system-audio.producer.default-device-name";

spl::shared_ptr<core::frame_consumer> create_consumer(const std::vector<std::wstring>&     params,
                                                      const core::video_format_repository& format_repository,
                                                      const std::vector<spl::shared_ptr<core::video_channel>>& channels,
                                                      const core::channel_info& channel_info)
{
    if (params.empty() || !(boost::iequals(params.at(0), L"AUDIO") || boost::iequals(params.at(0), L"SYSTEM-AUDIO")))
        return core::frame_consumer::empty();

    auto old_name = env::properties().get(old_config_path, L"");

    auto device_name = u8(get_param(L"DEVICE_NAME", params, old_name));
    auto delay = u8(get_param(L"DELAY", params, L"0"));
    return spl::make_shared<oal_consumer>(device_name, timespan{delay});
}

spl::shared_ptr<core::frame_consumer>
create_preconfigured_consumer(const boost::property_tree::wptree&                      ptree,
                              const core::video_format_repository&                     format_repository,
                              const std::vector<spl::shared_ptr<core::video_channel>>& channels,
                              const core::channel_info&                                channel_info)
{
    auto old_name = env::properties().get(old_config_path, L"");

    auto device_name = u8(ptree.get(L"device-name", old_name));
    auto delay = u8(ptree.get(L"delay", L"0"));
    return spl::make_shared<oal_consumer>(device_name, timespan{delay});
}

} // namespace caspar::oal
