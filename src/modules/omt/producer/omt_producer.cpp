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
#include "omt_producer.h"

#include "../util/omt_util.h"

#include <common/diagnostics/graph.h>
#include <common/log.h>
#include <common/os/thread.h>
#include <common/param.h>
#include <common/timer.h>
#include <common/utf.h>

#include <core/frame/draw_frame.h>
#include <core/frame/frame.h>
#include <core/frame/frame_factory.h>
#include <core/producer/frame_producer.h>
#include <core/video_format.h>

#include <boost/algorithm/string.hpp>

#pragma warning(push)
#pragma warning(disable : 4244)
extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}
#include "../../ffmpeg/util/av_util.h"
#pragma warning(pop)

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace caspar { namespace omt {

namespace {

// Frames waiting to be played. The oldest one is dropped when playback falls behind.
constexpr size_t max_queued_frames = 3;

// Audio that arrives without video to go with it is dropped beyond this length.
constexpr int max_buffered_audio_seconds = 2;

// True if the image holds every row that the frame announces.
bool is_usable_video(const OMTMediaFrame& frame)
{
    if (frame.Data == nullptr || frame.Width < 1 || frame.Height < 1) {
        return false;
    }

    const int64_t row_bytes = static_cast<int64_t>(frame.Width) * 4;
    return frame.Stride >= row_bytes && frame.DataLength >= static_cast<int64_t>(frame.Stride) * frame.Height;
}

// What a received audio frame may announce. libomt documents at most 32 channels, and passes the
// sample rate on without looking at it. A value outside of this range cannot come from a real sender.
constexpr int max_audio_channels    = 32;
constexpr int min_audio_sample_rate = 8000;
constexpr int max_audio_sample_rate = 384000;

// True if the frame can be converted without reading or allocating more than the data it holds.
bool is_usable_audio(const OMTMediaFrame& frame)
{
    if (frame.Data == nullptr || frame.Channels < 1 || frame.Channels > max_audio_channels ||
        frame.SamplesPerChannel < 1 || frame.SampleRate < min_audio_sample_rate ||
        frame.SampleRate > max_audio_sample_rate) {
        return false;
    }

    // libomt works out the size of the data in 32 bit arithmetic, which wraps around for very large counts.
    const int64_t announced_bytes = static_cast<int64_t>(frame.Channels) * frame.SamplesPerChannel * sizeof(float);
    return frame.DataLength >= announced_bytes;
}

struct swr_deleter
{
    void operator()(SwrContext* context) { swr_free(&context); }
};

void free_av_frame(AVFrame* frame) { av_frame_free(&frame); }

} // namespace

struct omt_producer : public core::frame_producer
{
    static std::atomic<int> instances_;
    const int               instance_no_;
    const std::wstring      name_;
    const std::string       name_utf8_;

    spl::shared_ptr<core::frame_factory> frame_factory_;
    core::video_format_desc              format_desc_;

    omt_lib*       lib_  = nullptr;
    omt_receive_t* recv_ = nullptr;

    std::atomic<bool> running_{false};
    std::thread       worker_thread_;

    std::queue<core::draw_frame> frames_;
    mutable std::mutex           frames_mutex_;
    core::draw_frame             last_frame_;

    // Converts the received planar float audio to CasparCG's interleaved 32 bit integers.
    std::unique_ptr<SwrContext, swr_deleter> swr_;
    int                                      recv_channels_    = 0;
    int                                      recv_sample_rate_ = 0;

    // Converted audio waiting for the next video frame to be played with.
    std::deque<int32_t> audio_queue_;
    std::mutex          audio_mutex_;
    bool                warned_about_audio_ = false;

    // CasparCG video frames need a different number of audio samples depending on the frame rate.
    int       cadence_counter_ = 0;
    const int cadence_length_;

    spl::shared_ptr<diagnostics::graph> graph_;
    caspar::timer                       frame_timer_;

  public:
    explicit omt_producer(spl::shared_ptr<core::frame_factory> frame_factory,
                          core::video_format_desc              format_desc,
                          std::wstring                         name)
        : instance_no_(instances_++)
        , name_(name)
        , name_utf8_(u8(name))
        , frame_factory_(frame_factory)
        , format_desc_(format_desc)
        , cadence_length_(static_cast<int>(format_desc.audio_cadence.size()))
    {
        lib_ = omt::load_library();

        graph_ = spl::make_shared<diagnostics::graph>();
        graph_->set_text(print());
        graph_->set_color("frame-time", diagnostics::color(0.5f, 1.0f, 0.2f));
        graph_->set_color("dropped-frame", diagnostics::color(0.3f, 0.6f, 0.3f));
        diagnostics::register_graph(graph_);

        running_       = true;
        worker_thread_ = std::thread([this] { run(); });
    }

    ~omt_producer()
    {
        running_ = false;
        if (worker_thread_.joinable() == true) {
            worker_thread_.join();
        }
    }

    // --- core::frame_producer ---

    std::wstring print() const override { return L"omt[" + std::to_wstring(instance_no_) + L"|" + name_ + L"]"; }

    std::wstring name() const override { return L"omt"; }

    core::draw_frame receive_impl(const core::video_field /*field*/, int /*nb_samples*/) override
    {
        std::lock_guard<std::mutex> lock(frames_mutex_);

        if (frames_.empty() == false) {
            last_frame_ = frames_.front();
            frames_.pop();
        }
        return last_frame_;
    }

    core::draw_frame last_frame(const core::video_field field) override
    {
        const bool has_last_frame = static_cast<bool>(last_frame_);
        if (has_last_frame == false) {
            last_frame_ = receive_impl(field, 0);
        }
        return core::draw_frame::still(last_frame_);
    }

    bool is_ready() override
    {
        std::lock_guard<std::mutex> lock(frames_mutex_);

        const bool has_last_frame = static_cast<bool>(last_frame_);
        return frames_.empty() == false || has_last_frame == true;
    }

    core::monitor::state state() const override
    {
        core::monitor::state state;
        state["omt/name"] = u8(name_);
        return state;
    }

  private:
    // --- Receive thread ---

    void run()
    {
        set_thread_name(L"OMT-RECV: " + name_);

        constexpr auto frame_types = static_cast<OMTFrameType>(OMTFrameType_Video | OMTFrameType_Audio);

        {
            auto lock = omt::serialize_create_call();
            recv_     = lib_->receive_create(
                name_utf8_.c_str(), frame_types, OMTPreferredVideoFormat_BGRA, OMTReceiveFlags_None);
        }

        if (recv_ == nullptr) {
            CASPAR_LOG(error) << print() << L" Failed to create OMT receiver.";
            return;
        }

        while (running_ == true) {
            const OMTMediaFrame* const frame = lib_->receive(recv_, frame_types, 100);
            if (frame == nullptr) {
                continue;
            }

            // An exception that leaves this thread would end the whole process.
            try {
                if (frame->Type == OMTFrameType_Video) {
                    handle_video(*frame);
                } else if (frame->Type == OMTFrameType_Audio) {
                    handle_audio(*frame);
                }
            } catch (...) {
                CASPAR_LOG_CURRENT_EXCEPTION();
            }
        }

        {
            auto lock = omt::serialize_create_call();
            lib_->receive_destroy(recv_);
        }
        recv_ = nullptr;
    }

    // --- Audio ---

    // (Re)creates the audio converter when the sender changes its channel count or sample rate.
    // Returns false if there is no converter for this audio.
    bool ensure_audio_converter(int channels, int sample_rate)
    {
        const bool converter_matches =
            swr_ != nullptr && recv_channels_ == channels && recv_sample_rate_ == sample_rate;
        if (converter_matches == true) {
            return true;
        }

        // The queued samples are laid out for the old channel count.
        {
            std::lock_guard<std::mutex> lock(audio_mutex_);
            audio_queue_.clear();
        }
        swr_.reset();
        recv_channels_    = 0;
        recv_sample_rate_ = 0;

        AVChannelLayout layout;
        av_channel_layout_default(&layout, channels);

        SwrContext* context = nullptr;
        const int   result  = swr_alloc_set_opts2(&context,
                                               &layout,
                                               AV_SAMPLE_FMT_S32,
                                               format_desc_.audio_sample_rate,
                                               &layout,
                                               AV_SAMPLE_FMT_FLTP,
                                               sample_rate,
                                               0,
                                               nullptr);
        av_channel_layout_uninit(&layout);
        if (result < 0) {
            return false;
        }
        if (swr_init(context) < 0) {
            swr_free(&context);
            return false;
        }

        swr_.reset(context);
        recv_channels_    = channels;
        recv_sample_rate_ = sample_rate;
        return true;
    }

    // Converts received audio and queues it for the next video frames.
    void handle_audio(const OMTMediaFrame& frame)
    {
        if (is_usable_audio(frame) == false) {
            if (warned_about_audio_ == false) {
                warned_about_audio_ = true;
                CASPAR_LOG(warning) << print() << L" Ignoring audio that the sender announced as " << frame.Channels
                                    << L" channels, " << frame.SampleRate << L" Hz, " << frame.SamplesPerChannel
                                    << L" samples per channel and " << frame.DataLength << L" bytes.";
            }
            return;
        }

        if (ensure_audio_converter(frame.Channels, frame.SampleRate) == false) {
            return;
        }

        // Planar: the samples of each channel follow each other, one channel after the other.
        std::vector<const uint8_t*> input_planes(frame.Channels);
        for (int channel = 0; channel < frame.Channels; ++channel) {
            const size_t channel_offset = static_cast<size_t>(channel) * frame.SamplesPerChannel * sizeof(float);
            input_planes[channel]       = reinterpret_cast<const uint8_t*>(frame.Data) + channel_offset;
        }

        const int64_t max_output_samples =
            av_rescale_rnd(swr_get_delay(swr_.get(), recv_sample_rate_) + frame.SamplesPerChannel,
                           format_desc_.audio_sample_rate,
                           recv_sample_rate_,
                           AV_ROUND_UP);
        if (max_output_samples <= 0) {
            return;
        }

        std::vector<int32_t> output(static_cast<size_t>(max_output_samples) * frame.Channels);
        uint8_t*             output_plane = reinterpret_cast<uint8_t*>(output.data());

        const int converted_samples = swr_convert(swr_.get(),
                                                  &output_plane,
                                                  static_cast<int>(max_output_samples),
                                                  input_planes.data(),
                                                  frame.SamplesPerChannel);
        if (converted_samples <= 0) {
            return;
        }

        std::lock_guard<std::mutex> lock(audio_mutex_);
        audio_queue_.insert(audio_queue_.end(),
                            output.begin(),
                            output.begin() + static_cast<size_t>(converted_samples) * frame.Channels);

        const size_t max_queued_samples =
            static_cast<size_t>(format_desc_.audio_sample_rate) * frame.Channels * max_buffered_audio_seconds;
        while (audio_queue_.size() > max_queued_samples) {
            audio_queue_.pop_front();
        }
    }

    // Builds the audio for one video frame from the queue: as many samples as the frame rate
    // calls for, with silence for what has not arrived yet.
    std::shared_ptr<AVFrame> take_audio_for_video_frame(int channels)
    {
        const int wanted_samples = format_desc_.audio_cadence[cadence_counter_ % cadence_length_];
        cadence_counter_         = (cadence_counter_ + 1) % cadence_length_;

        std::vector<int32_t> samples(static_cast<size_t>(wanted_samples) * channels, 0);
        {
            std::lock_guard<std::mutex> lock(audio_mutex_);

            const int available_samples = channels > 0 ? static_cast<int>(audio_queue_.size() / channels) : 0;
            const int samples_to_take   = std::min(available_samples, wanted_samples);
            for (int index = 0; index < samples_to_take * channels; ++index) {
                samples[index] = audio_queue_.front();
                audio_queue_.pop_front();
            }
        }

        std::shared_ptr<AVFrame> audio_frame(av_frame_alloc(), free_av_frame);
        av_channel_layout_default(&audio_frame->ch_layout, channels);
        audio_frame->format      = AV_SAMPLE_FMT_S32;
        audio_frame->sample_rate = format_desc_.audio_sample_rate;
        audio_frame->nb_samples  = wanted_samples;

        if (wanted_samples > 0 && av_frame_get_buffer(audio_frame.get(), 32) == 0) {
            std::memcpy(audio_frame->data[0], samples.data(), samples.size() * sizeof(int32_t));
        }
        return audio_frame;
    }

    // --- Video ---

    // Copies the received BGRA image into a frame of its own, because libomt reuses its buffer.
    std::shared_ptr<AVFrame> copy_video_frame(const OMTMediaFrame& frame)
    {
        std::shared_ptr<AVFrame> video_frame(av_frame_alloc(), free_av_frame);
        video_frame->format = AV_PIX_FMT_BGRA;
        video_frame->width  = frame.Width;
        video_frame->height = frame.Height;

        if (av_frame_get_buffer(video_frame.get(), 32) < 0) {
            return nullptr;
        }

        const uint8_t* const source = reinterpret_cast<const uint8_t*>(frame.Data);
        for (int row = 0; row < frame.Height; ++row) {
            std::memcpy(video_frame->data[0] + static_cast<size_t>(row) * video_frame->linesize[0],
                        source + static_cast<size_t>(row) * frame.Stride,
                        static_cast<size_t>(frame.Width) * 4);
        }
        return video_frame;
    }

    void handle_video(const OMTMediaFrame& frame)
    {
        if (is_usable_video(frame) == false) {
            return;
        }

        frame_timer_.restart();

        auto video_frame = copy_video_frame(frame);
        if (video_frame == nullptr) {
            return;
        }

        const int channels    = recv_channels_ > 0 ? recv_channels_ : format_desc_.audio_channels;
        auto      audio_frame = take_audio_for_video_frame(channels);

        // CasparCG expects premultiplied alpha, so the frame has to be marked if the sender used straight alpha.
        const bool has_alpha         = (frame.Flags & OMTVideoFlags_Alpha) != 0;
        const bool is_premultiplied  = (frame.Flags & OMTVideoFlags_PreMultiplied) != 0;
        const bool is_straight_alpha = has_alpha == true && is_premultiplied == false;

        auto mutable_frame = ffmpeg::make_frame(this,
                                                *frame_factory_,
                                                std::move(video_frame),
                                                std::move(audio_frame),
                                                core::color_space::unknown,
                                                core::frame_geometry::scale_mode::stretch,
                                                is_straight_alpha);

        {
            std::lock_guard<std::mutex> lock(frames_mutex_);

            frames_.push(core::draw_frame(std::move(mutable_frame)));
            while (frames_.size() > max_queued_frames) {
                frames_.pop();
                graph_->set_tag(diagnostics::tag_severity::WARNING, "dropped-frame");
            }
        }

        graph_->set_value("frame-time", frame_timer_.elapsed() * format_desc_.fps * 0.5);
    }
};

std::atomic<int> omt_producer::instances_(0);

namespace {

// The source can be written as "[OMT] name", "OMT name" or "omt://host:port". The name is the one
// that OMT LIST prints. A URL is passed on as it is, because libomt only connects directly to an
// address that starts with omt://, and looks any other address up among the discovered sources.
// Returns an empty string if `params` does not ask for an OMT source.
std::wstring parse_source_name(const std::vector<std::wstring>& params)
{
    const std::wstring& first = params.at(0);

    if (boost::istarts_with(first, L"omt://") == true) {
        return first;
    }

    const bool is_bracketed_keyword = boost::iequals(first, L"[OMT]") == true;
    const bool is_plain_keyword     = boost::iequals(first, L"OMT") == true;
    if ((is_bracketed_keyword == true || is_plain_keyword == true) && params.size() > 1) {
        return params.at(1);
    }

    return L"";
}

} // namespace

spl::shared_ptr<core::frame_producer> create_omt_producer(const core::frame_producer_dependencies& dependencies,
                                                          const std::vector<std::wstring>&         params)
{
    if (params.empty() == true) {
        return core::frame_producer::empty();
    }

    const std::wstring source_name = parse_source_name(params);
    if (source_name.empty() == true) {
        return core::frame_producer::empty();
    }

    return spl::make_shared<omt_producer>(dependencies.frame_factory, dependencies.format_desc, source_name);
}

}} // namespace caspar::omt
