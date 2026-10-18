/*
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
 */

#include "proaudio_producer.h"

#include "../util/device_util.h"
#include "../util/ring_buffer.h"

#include <common/array.h>
#include <common/diagnostics/graph.h>
#include <common/except.h>
#include <common/log.h>
#include <common/param.h>
#include <common/timer.h>
#include <common/utf.h>

#include <core/frame/draw_frame.h>
#include <core/frame/frame.h>
#include <core/frame/frame_factory.h>
#include <core/frame/pixel_format.h>
#include <core/producer/frame_producer.h>
#include <core/video_format.h>

#include <boost/algorithm/string.hpp>

#include <portaudio.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace caspar { namespace proaudio {

struct proaudio_producer : public core::frame_producer
{
    spl::shared_ptr<diagnostics::graph> graph_;
    spl::shared_ptr<core::frame_factory> frame_factory_;
    core::video_format_desc              format_desc_;

    std::shared_ptr<pa_library> library_;
    std::string                       device_name_;
    std::string                       host_api_name_;
    std::string                       channel_map_param_;
    std::vector<int>                  channel_map_; // 1-based source device input channel per casparcg channel
    int                                buffer_size_frames_;
    double                             latency_ms_;

    PaStream* stream_          = nullptr;
    int       device_channels_ = 0;

    unsigned long              max_frames_per_callback_ = 8192;
    std::vector<std::int16_t> scratch_; // pre-sized once, only touched by the realtime callback
    std::string                device_name_for_print_; // set once in the ctor, only read after

    std::unique_ptr<frame_ring_buffer> ring_;
    std::atomic<bool>                  dropout_{false};
    std::atomic<bool>                  device_overflow_{false};

  public:
    explicit proaudio_producer(spl::shared_ptr<core::frame_factory> frame_factory,
                               core::video_format_desc              format_desc,
                               std::string                          device_name,
                               std::string                          host_api_name,
                               std::string                          channel_map,
                               int                                  buffer_size_frames,
                               double                               latency_ms)
        : frame_factory_(std::move(frame_factory))
        , format_desc_(std::move(format_desc))
        , library_(pa_library::acquire())
        , device_name_(std::move(device_name))
        , host_api_name_(std::move(host_api_name))
        , channel_map_param_(std::move(channel_map))
        , buffer_size_frames_(buffer_size_frames)
        , latency_ms_(latency_ms)
    {
        using diagnostics::color;
        graph_->set_color("underflow", color(0.6, 0.3, 0.9));
        graph_->set_color("device-overflow", color(0.6, 0.3, 0.3));
        graph_->set_color("dropout", color(0.9, 0.6, 0.3));
        diagnostics::register_graph(graph_);

        channel_map_ = parse_channel_map(channel_map_param_, format_desc_.audio_channels);
        if (channel_map_.empty())
            CASPAR_THROW_EXCEPTION(invalid_operation() << msg_info("CHANNEL_MAP resolved to no channels"));

        for (auto ch : channel_map_) {
            if (ch < 1)
                CASPAR_THROW_EXCEPTION(
                    invalid_operation() << msg_info("CHANNEL_MAP entries must be 1-based channel numbers, got " +
                                                    std::to_string(ch)));
        }

        auto device = find_device(device_direction::input, device_name_, host_api_name_);
        auto info   = Pa_GetDeviceInfo(device);
        if (!info)
            CASPAR_THROW_EXCEPTION(invalid_operation()
                                   << msg_info("No default PortAudio input device is available on this machine. "
                                               "Specify DEVICE_NAME/HOST_API explicitly."));

        device_name_for_print_ = info->name;

        auto max_source_channel = *std::max_element(channel_map_.begin(), channel_map_.end());
        if (max_source_channel > info->maxInputChannels)
            CASPAR_THROW_EXCEPTION(
                invalid_operation() << msg_info("CHANNEL_MAP requires input channel " +
                                                std::to_string(max_source_channel) + " but device '" + info->name +
                                                "' only has " + std::to_string(info->maxInputChannels) +
                                                " input channels"));

        if (channel_map_.size() > static_cast<std::size_t>(format_desc_.audio_channels))
            CASPAR_THROW_EXCEPTION(invalid_operation()
                                   << msg_info("CHANNEL_MAP contains more channels than the format's " +
                                               std::to_string(format_desc_.audio_channels) + " audio channels"));

        device_channels_ = max_source_channel;

        if (buffer_size_frames_ > 0)
            max_frames_per_callback_ = static_cast<unsigned long>(std::clamp(buffer_size_frames_, 16, 65536));

        auto max_cadence = *std::max_element(format_desc_.audio_cadence.begin(), format_desc_.audio_cadence.end());

        // At least double the largest block the receive_impl side can ask for in one go (one video
        // frame's worth of samples) so a slow tick can't starve the ring dry and report a false
        // underflow, mirroring the consumer's ring sizing.
        auto ring_capacity = std::max<std::size_t>(static_cast<std::size_t>(max_cadence) * 4,
                                                   static_cast<std::size_t>(max_frames_per_callback_) * 2);

        ring_ = std::make_unique<frame_ring_buffer>(ring_capacity, channel_map_.size());
        scratch_.resize(max_frames_per_callback_ * device_channels_);

        PaStreamParameters in_params{};
        in_params.device                    = device;
        in_params.channelCount               = device_channels_;
        in_params.sampleFormat               = paInt16;
        in_params.suggestedLatency = latency_ms_ >= 0.0 ? latency_ms_ / 1000.0 : info->defaultLowInputLatency;
        in_params.hostApiSpecificStreamInfo = nullptr;

        auto err = Pa_OpenStream(&stream_,
                                 &in_params,
                                 nullptr,
                                 format_desc_.audio_sample_rate,
                                 buffer_size_frames_ > 0 ? max_frames_per_callback_ : paFramesPerBufferUnspecified,
                                 paNoFlag,
                                 &proaudio_producer::pa_callback,
                                 this);
        if (err != paNoError)
            CASPAR_THROW_EXCEPTION(invalid_operation()
                                   << msg_info(std::string("Failed to open PortAudio stream: ") +
                                               Pa_GetErrorText(err)));

        err = Pa_StartStream(stream_);
        if (err != paNoError) {
            auto message   = std::string("Failed to start PortAudio stream: ") + Pa_GetErrorText(err);
            auto close_err = Pa_CloseStream(stream_);
            stream_        = nullptr;
            if (close_err != paNoError)
                message += std::string("; failed to close PortAudio stream: ") + Pa_GetErrorText(close_err);
            CASPAR_THROW_EXCEPTION(invalid_operation() << msg_info(message));
        }

        graph_->set_text(print());
        CASPAR_LOG(info) << print() << L" Using device '" << u16(std::string(info->name)) << "'";
    }

    ~proaudio_producer() override
    {
        if (stream_) {
            Pa_StopStream(stream_);
            Pa_CloseStream(stream_);
            stream_ = nullptr;
        }
    }

    // frame_producer

    core::draw_frame receive_impl(const core::video_field field, int nb_samples) override
    {
        auto n_map = channel_map_.size();

        std::vector<std::int16_t> captured(static_cast<std::size_t>(nb_samples) * n_map);
        auto                       popped = ring_->pop(captured.data(), static_cast<std::size_t>(nb_samples));
        if (popped < static_cast<std::size_t>(nb_samples))
            graph_->set_tag(diagnostics::tag_severity::WARNING, "underflow");
        if (device_overflow_.exchange(false, std::memory_order_relaxed))
            graph_->set_tag(diagnostics::tag_severity::WARNING, "device-overflow");
        if (dropout_.exchange(false, std::memory_order_relaxed))
            graph_->set_tag(diagnostics::tag_severity::WARNING, "dropout");

        // Left-justify each 16-bit capture into caspar's 32-bit sample range - the inverse of the
        // consumer's `>> 16` truncation down to int16 for playback.
        auto                      output_channels = static_cast<std::size_t>(format_desc_.audio_channels);
        std::vector<std::int32_t> audio(static_cast<std::size_t>(nb_samples) * output_channels, 0);
        for (int sample = 0; sample < nb_samples; ++sample) {
            for (std::size_t channel = 0; channel < n_map; ++channel) {
                auto source   = static_cast<std::size_t>(sample) * n_map + channel;
                auto target   = static_cast<std::size_t>(sample) * output_channels + channel;
                audio[target] = static_cast<std::int32_t>(captured[source]) << 16;
            }
        }

        // A 1x1 fully transparent pixel, same trick as color_producer's "EMPTY" color - contributes
        // nothing visually so this producer is audio-only, but stays a valid (non-invalid-format)
        // frame so its audio still reaches the mixer.
        core::pixel_format_desc desc(core::pixel_format::bgra);
        desc.planes.push_back(core::pixel_format_desc::plane(1, 1, 4));

        auto frame                                              = frame_factory_->create_frame(this, desc);
        *reinterpret_cast<std::uint32_t*>(frame.image_data(0).begin()) = 0x00000000;
        frame.audio_data()                                      = array<std::int32_t>(std::move(audio));

        return core::draw_frame(std::move(frame));
    }

    std::wstring print() const override
    {
        return L"proaudio[" + u16(device_name_for_print_) + L"|" + format_desc_.name + L"]";
    }

    std::wstring name() const override { return L"proaudio"; }

    core::monitor::state state() const override
    {
        static const core::monitor::state empty;
        return empty;
    }

    bool is_ready() override { return true; }

  private:
    // Realtime audio thread - no locks, no allocation, no logging in here.
    static int pa_callback(const void*                      input,
                           void* /*output*/,
                           unsigned long                    frame_count,
                           const PaStreamCallbackTimeInfo* /*time_info*/,
                           PaStreamCallbackFlags            status_flags,
                           void* user_data)
    {
        auto self = static_cast<proaudio_producer*>(user_data);
        auto in   = static_cast<const std::int16_t*>(input);

        // The driver/device itself dropped samples before we even got them - distinct from (and a
        // likely cause of) our own ring_ underflow below.
        if (status_flags & paInputOverflow)
            self->device_overflow_.store(true, std::memory_order_relaxed);

        auto frames = std::min<unsigned long>(frame_count, self->max_frames_per_callback_);
        if (frames < frame_count)
            self->dropout_.store(true, std::memory_order_relaxed);

        auto n_map = self->channel_map_.size();

        if (in) {
            for (unsigned long i = 0; i < frames; ++i) {
                for (std::size_t c = 0; c < n_map; ++c) {
                    auto src_ch = static_cast<std::size_t>(self->channel_map_[c] - 1); // 1-based -> 0-based
                    self->scratch_[i * n_map + c] = in[i * self->device_channels_ + src_ch];
                }
            }
        } else {
            std::fill(self->scratch_.begin(), self->scratch_.begin() + frames * n_map, 0);
        }

        auto written = self->ring_->push(self->scratch_.data(), frames);
        if (written < frames)
            self->dropout_.store(true, std::memory_order_relaxed);

        return paContinue;
    }
};

spl::shared_ptr<core::frame_producer> create_producer(const core::frame_producer_dependencies& dependencies,
                                                      const std::vector<std::wstring>&         params)
{
    if (params.empty() || !boost::iequals(params.at(0), L"PROAUDIO"))
        return core::frame_producer::empty();

    auto device_name  = u8(get_param(L"DEVICE_NAME", params, L""));
    auto host_api     = u8(get_param(L"HOST_API", params, L""));
    auto channel_map  = u8(get_param(L"CHANNEL_MAP", params, L""));
    auto buffer_size  = get_param(L"BUFFER_SIZE", params, -1);
    auto latency_ms   = get_param(L"LATENCY_MS", params, -1.0);

    return spl::make_shared<proaudio_producer>(
        dependencies.frame_factory, dependencies.format_desc, device_name, host_api, channel_map, buffer_size, latency_ms);
}

}} // namespace caspar::proaudio
