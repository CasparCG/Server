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

#include "../StdAfx.h"

#include "decklink_producer.h"

#include "../util/util.h"

#include <common/diagnostics/graph.h>
#include <common/except.h>
#include <common/executor.h>
#include <common/log.h>
#include <common/param.h>
#include <common/scope_exit.h>
#include <common/timer.h>

#include <ffmpeg/util/av_assert.h>
#include <ffmpeg/util/av_util.h>

#include <core/diagnostics/call_context.h>
#include <core/frame/draw_frame.h>
#include <core/frame/frame.h>
#include <core/frame/frame_factory.h>
#include <core/frame/frame_timecode.h>
#include <core/frame/frame_transform.h>
#include <core/frame/pixel_format.h>
#include <core/mixer/audio/audio_mixer.h>
#include <core/monitor/monitor.h>
#include <core/producer/frame_producer.h>

#include <tbb/concurrent_queue.h>

#include <boost/algorithm/string.hpp>
#include <boost/circular_buffer.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/range/adaptor/transformed.hpp>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4244)
#endif
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
#include <libavutil/timecode.h>
}
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <boost/format.hpp>

#include "../decklink_api.h"

#include <functional>

using namespace caspar::ffmpeg;

namespace caspar { namespace decklink {

template <typename T>
std::wstring to_string(const T& cadence)
{
    return boost::join(
        cadence | boost::adaptors::transformed([](size_t i) { return boost::lexical_cast<std::wstring>(i); }), L", ");
}

struct Filter
{
    std::shared_ptr<AVFilterGraph> graph        = nullptr;
    AVFilterContext*               sink         = nullptr;
    AVFilterContext*               video_source = nullptr;
    AVFilterContext*               audio_source = nullptr;

    Filter(std::string                    filter_spec,
           AVMediaType                    type,
           const core::video_format_desc& format_desc,
           com_ptr<IDeckLinkDisplayMode>  dm)
    {
        if (type == AVMEDIA_TYPE_VIDEO) {
            if (filter_spec.empty()) {
                filter_spec = "null";
            }

            {
                std::string colorspace = ""; // TODO (fix) 6-625 or 6-525?
                if (dm->GetFlags() & bmdDisplayModeColorspaceRec601) {
                    colorspace = "bt601-6-625";
                } else if (dm->GetFlags() & bmdDisplayModeColorspaceRec709) {
                    colorspace = "bt709";
                }
                // TODO (fix) bmd doesn't have 2020 in flags?
                // else if (dm->GetFlags() & bmdDisplayModeColorspaceRec2020) {
                //}
                if (!colorspace.empty()) {
                    filter_spec =
                        (boost::format("colorspace=iall=%s:all=%s,") % colorspace % colorspace).str() + filter_spec;
                }
            }

            if (format_desc.field_count == 2) {
                filter_spec += ",bwdif=mode=send_field:parity=auto:deint=all";
            }

            filter_spec +=
                (boost::format(",fps=%d/%d") % format_desc.framerate.numerator() % format_desc.framerate.denominator())
                    .str();
        } else {
            if (filter_spec.empty()) {
                filter_spec = "anull";
            }
            filter_spec +=
                (boost::format(",aresample=sample_rate=%d:async=2000") % format_desc.audio_sample_rate).str();
        }

        AVFilterInOut* outputs = nullptr;
        AVFilterInOut* inputs  = nullptr;

        CASPAR_SCOPE_EXIT
        {
            avfilter_inout_free(&inputs);
            avfilter_inout_free(&outputs);
        };

        int video_input_count = 0;
        int audio_input_count = 0;
        {
            auto graph2 = avfilter_graph_alloc();
            if (!graph2) {
                FF_RET(AVERROR(ENOMEM), "avfilter_graph_alloc");
            }

            CASPAR_SCOPE_EXIT
            {
                avfilter_graph_free(&graph2);
                avfilter_inout_free(&inputs);
                avfilter_inout_free(&outputs);
            };

            FF(avfilter_graph_parse2(graph2, filter_spec.c_str(), &inputs, &outputs));

            for (auto cur = inputs; cur; cur = cur->next) {
                const auto filter_type = avfilter_pad_get_type(cur->filter_ctx->input_pads, cur->pad_idx);
                if (filter_type == AVMEDIA_TYPE_VIDEO) {
                    video_input_count += 1;
                } else if (filter_type == AVMEDIA_TYPE_AUDIO) {
                    audio_input_count += 1;
                }
            }
        }

        graph = std::shared_ptr<AVFilterGraph>(avfilter_graph_alloc(),
                                               [](AVFilterGraph* ptr) { avfilter_graph_free(&ptr); });

        if (!graph) {
            FF_RET(AVERROR(ENOMEM), "avfilter_graph_alloc");
        }

        graph->nb_threads = 16;
        graph->execute    = graph_execute;

        FF(avfilter_graph_parse2(graph.get(), filter_spec.c_str(), &inputs, &outputs));

        for (auto cur = inputs; cur; cur = cur->next) {
            const auto filter_type = avfilter_pad_get_type(cur->filter_ctx->input_pads, cur->pad_idx);

            if (filter_type == AVMEDIA_TYPE_VIDEO) {
                if (video_source) {
                    CASPAR_THROW_EXCEPTION(ffmpeg_error_t() << boost::errinfo_errno(EINVAL)
                                                            << msg_info_t("only single video input supported"));
                }
                const auto sar = boost::rational<int>(format_desc.square_width, format_desc.square_height) /
                                 boost::rational<int>(format_desc.width, format_desc.height);

                auto args = (boost::format("video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:sar=%d/%d:frame_rate=%d/%d") %
                             format_desc.width % format_desc.height % AV_PIX_FMT_UYVY422 % 1 % AV_TIME_BASE %
                             sar.numerator() % sar.denominator() % format_desc.framerate.numerator() %
                             (format_desc.framerate.denominator() * format_desc.field_count))
                                .str();
                auto name = (boost::format("in_%d") % 0).str();

                FF(avfilter_graph_create_filter(
                    &video_source, avfilter_get_by_name("buffer"), name.c_str(), args.c_str(), nullptr, graph.get()));
                FF(avfilter_link(video_source, 0, cur->filter_ctx, cur->pad_idx));
            } else if (filter_type == AVMEDIA_TYPE_AUDIO) {
                if (audio_source) {
                    CASPAR_THROW_EXCEPTION(ffmpeg_error_t() << boost::errinfo_errno(EINVAL)
                                                            << msg_info_t("only single audio input supported"));
                }

                auto args = (boost::format("time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=%#x") % 1 %
                             format_desc.audio_sample_rate % format_desc.audio_sample_rate % AV_SAMPLE_FMT_S32 %
                             av_get_default_channel_layout(format_desc.audio_channels))
                                .str();
                auto name = (boost::format("in_%d") % 0).str();

                FF(avfilter_graph_create_filter(
                    &audio_source, avfilter_get_by_name("abuffer"), name.c_str(), args.c_str(), nullptr, graph.get()));
                FF(avfilter_link(audio_source, 0, cur->filter_ctx, cur->pad_idx));
            } else {
                CASPAR_THROW_EXCEPTION(ffmpeg_error_t() << boost::errinfo_errno(EINVAL)
                                                        << msg_info_t("only video and audio filters supported"));
            }
        }

        if (type == AVMEDIA_TYPE_VIDEO) {
            FF(avfilter_graph_create_filter(
                &sink, avfilter_get_by_name("buffersink"), "out", nullptr, nullptr, graph.get()));

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4245)
#endif
            AVPixelFormat pix_fmts[] = {AV_PIX_FMT_BGRA, AV_PIX_FMT_NONE};
            FF(av_opt_set_int_list(sink, "pix_fmts", pix_fmts, -1, AV_OPT_SEARCH_CHILDREN));
#ifdef _MSC_VER
#pragma warning(pop)
#endif
        } else if (type == AVMEDIA_TYPE_AUDIO) {
            FF(avfilter_graph_create_filter(
                &sink, avfilter_get_by_name("abuffersink"), "out", nullptr, nullptr, graph.get()));
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4245)
#endif
            AVSampleFormat sample_fmts[]     = {AV_SAMPLE_FMT_S32, AV_SAMPLE_FMT_NONE};
            int64_t        channel_layouts[] = {av_get_default_channel_layout(format_desc.audio_channels), 0};
            int            sample_rates[]    = {format_desc.audio_sample_rate, 0};
            FF(av_opt_set_int_list(sink, "sample_fmts", sample_fmts, -1, AV_OPT_SEARCH_CHILDREN));
            FF(av_opt_set_int_list(sink, "channel_layouts", channel_layouts, 0, AV_OPT_SEARCH_CHILDREN));
            FF(av_opt_set_int_list(sink, "sample_rates", sample_rates, 0, AV_OPT_SEARCH_CHILDREN));
#ifdef _MSC_VER
#pragma warning(pop)
#endif
        } else {
            CASPAR_THROW_EXCEPTION(ffmpeg_error_t()
                                   << boost::errinfo_errno(EINVAL) << msg_info_t("invalid output media type"));
        }

        {
            const auto cur = outputs;

            if (!cur || cur->next) {
                CASPAR_THROW_EXCEPTION(ffmpeg_error_t() << boost::errinfo_errno(EINVAL)
                                                        << msg_info_t("invalid filter graph output count"));
            }

            if (avfilter_pad_get_type(cur->filter_ctx->output_pads, cur->pad_idx) != type) {
                CASPAR_THROW_EXCEPTION(ffmpeg_error_t() << boost::errinfo_errno(EINVAL)
                                                        << msg_info_t("invalid filter output media type"));
            }

            FF(avfilter_link(cur->filter_ctx, cur->pad_idx, sink, 0));
        }

        FF(avfilter_graph_config(graph.get(), nullptr));
    }
};

class decklink_producer : public IDeckLinkInputCallback
{
    const int                           device_index_;
    core::monitor::state                state_;
    mutable std::mutex                  state_mutex_;
    spl::shared_ptr<diagnostics::graph> graph_;
    caspar::timer                       tick_timer_;

    com_ptr<IDeckLink>                 decklink_   = get_device(device_index_);
    com_iface_ptr<IDeckLinkInput>      input_      = iface_cast<IDeckLinkInput>(decklink_);
    com_iface_ptr<IDeckLinkAttributes> attributes_ = iface_cast<IDeckLinkAttributes>(decklink_);

    const std::wstring model_name_ = get_model_name(decklink_);

    core::video_format_desc              format_desc_;
    std::vector<int>                     audio_cadence_ = format_desc_.audio_cadence;
    spl::shared_ptr<core::frame_factory> frame_factory_;

    double in_sync_  = 0.0;
    double out_sync_ = 0.0;

    bool freeze_on_lost_;
    bool has_signal_;

    tbb::concurrent_bounded_queue<std::pair<core::frame_timecode, core::draw_frame>> frame_buffer_;
    std::pair<core::frame_timecode, core::draw_frame>                                last_frame_;

    std::exception_ptr exception_;

    com_ptr<IDeckLinkDisplayMode> mode_ =
        get_display_mode(input_, format_desc_.format, bmdFormat8BitBGRA, bmdVideoOutputFlagDefault);
    int field_count_ = mode_->GetFieldDominance() != bmdProgressiveFrame ? 2 : 1;

    Filter video_filter_;
    Filter audio_filter_;

  public:
    decklink_producer(const core::video_format_desc&              format_desc,
                      int                                         device_index,
                      const spl::shared_ptr<core::frame_factory>& frame_factory,
                      const std::string&                          vfilter,
                      const std::string&                          afilter,
                      bool                                        freeze_on_lost)
        : device_index_(device_index)
        , format_desc_(format_desc)
        , frame_factory_(frame_factory)
        , freeze_on_lost_(freeze_on_lost)
        , video_filter_(vfilter, AVMEDIA_TYPE_VIDEO, format_desc_, mode_)
        , audio_filter_(afilter, AVMEDIA_TYPE_AUDIO, format_desc_, mode_)
    {
        boost::range::rotate(audio_cadence_, std::end(audio_cadence_) - 1);

        frame_buffer_.set_capacity(field_count_ + 1);

        graph_->set_color("tick-time", diagnostics::color(0.0f, 0.6f, 0.9f));
        graph_->set_color("late-frame", diagnostics::color(0.6f, 0.3f, 0.3f));
        graph_->set_color("frame-time", diagnostics::color(1.0f, 0.0f, 0.0f));
        graph_->set_color("dropped-frame", diagnostics::color(0.3f, 0.6f, 0.3f));
        graph_->set_color("output-buffer", diagnostics::color(0.0f, 1.0f, 0.0f));
        graph_->set_color("in-sync", diagnostics::color(1.0f, 0.2f, 0.0f));
        graph_->set_color("out-sync", diagnostics::color(0.0f, 0.2f, 1.0f));
        graph_->set_text(print());
        diagnostics::register_graph(graph_);

        if (FAILED(input_->EnableVideoInput(mode_->GetDisplayMode(), bmdFormat8BitYUV, 0))) {
            CASPAR_THROW_EXCEPTION(caspar_exception() << msg_info(print() + L" Could not enable video input.")
                                                      << boost::errinfo_api_function("EnableVideoInput"));
        }

        if (FAILED(input_->EnableAudioInput(bmdAudioSampleRate48kHz,
                                            bmdAudioSampleType32bitInteger,
                                            static_cast<int>(format_desc_.audio_channels)))) {
            CASPAR_THROW_EXCEPTION(caspar_exception() << msg_info(print() + L" Could not enable audio input.")
                                                      << boost::errinfo_api_function("EnableAudioInput"));
        }

        if (FAILED(input_->SetCallback(this)) != S_OK) {
            CASPAR_THROW_EXCEPTION(caspar_exception() << msg_info(print() + L" Failed to set input callback.")
                                                      << boost::errinfo_api_function("SetCallback"));
        }

        if (FAILED(input_->StartStreams())) {
            CASPAR_THROW_EXCEPTION(caspar_exception() << msg_info(print() + L" Failed to start input stream.")
                                                      << boost::errinfo_api_function("StartStreams"));
        }

        CASPAR_LOG(info) << print() << L" Initialized";
    }

    ~decklink_producer()
    {
        if (input_ != nullptr) {
            input_->StopStreams();
            input_->DisableVideoInput();
        }
    }

    virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, LPVOID*) { return E_NOINTERFACE; }
    virtual ULONG STDMETHODCALLTYPE AddRef() { return 1; }
    virtual ULONG STDMETHODCALLTYPE Release() { return 1; }

    virtual HRESULT STDMETHODCALLTYPE VideoInputFormatChanged(BMDVideoInputFormatChangedEvents /*notificationEvents*/,
                                                              IDeckLinkDisplayMode* newDisplayMode,
                                                              BMDDetectedVideoInputFormatFlags /*detectedSignalFlags*/)
    {
        return S_OK;
    }

    virtual HRESULT STDMETHODCALLTYPE VideoInputFrameArrived(IDeckLinkVideoInputFrame*  video,
                                                             IDeckLinkAudioInputPacket* audio)
    {
        caspar::timer frame_timer;

        CASPAR_SCOPE_EXIT
        {
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                state_["file/name"]              = model_name_;
                state_["file/path"]              = device_index_;
                state_["file/audio/sample-rate"] = format_desc_.audio_sample_rate;
                state_["file/audio/channels"]    = format_desc_.audio_channels;
                state_["file/fps"]               = format_desc_.fps;
                state_["profiler/time"]          = {frame_timer.elapsed(), format_desc_.fps};
                state_["buffer"]                 = {frame_buffer_.size(), frame_buffer_.capacity()};
                state_["has_signal"]             = has_signal_;

                if (video) {
                    state_["file/video/width"]  = video->GetWidth();
                    state_["file/video/height"] = video->GetHeight();
                }
            }

            graph_->set_value("frame-time", frame_timer.elapsed() * format_desc_.fps * 0.5);
            graph_->set_value("output-buffer",
                              static_cast<float>(frame_buffer_.size()) / static_cast<float>(frame_buffer_.capacity()));
        };

        try {
            graph_->set_value("tick-time", tick_timer_.elapsed() * format_desc_.fps * 0.5);
            tick_timer_.restart();

            BMDTimeValue in_video_pts = 0LL;
            BMDTimeValue in_audio_pts = 0LL;

            auto new_timecode = core::frame_timecode::empty();

            if (video) {
                const auto flags = video->GetFlags();
                has_signal_      = !(flags & bmdFrameHasNoInputSource);
                if (freeze_on_lost_ && !has_signal_) {
                    return S_OK;
                }

                auto src    = std::shared_ptr<AVFrame>(av_frame_alloc(), [](AVFrame* ptr) { av_frame_free(&ptr); });
                src->format = AV_PIX_FMT_UYVY422;
                src->width  = video->GetWidth();
                src->height = video->GetHeight();
                src->interlaced_frame = mode_->GetFieldDominance() != bmdProgressiveFrame;
                src->top_field_first  = mode_->GetFieldDominance() == bmdUpperFieldFirst ? 1 : 0;
                src->key_frame        = 1;

                void* video_bytes = nullptr;
                if (SUCCEEDED(video->GetBytes(&video_bytes)) && video_bytes) {
                    video->AddRef();
                    src = std::shared_ptr<AVFrame>(src.get(), [src, video](AVFrame* ptr) { video->Release(); });

                    src->data[0]     = reinterpret_cast<uint8_t*>(video_bytes);
                    src->linesize[0] = video->GetRowBytes();

                    BMDTimeValue duration;
                    if (SUCCEEDED(video->GetStreamTime(&in_video_pts, &duration, AV_TIME_BASE))) {
                        src->pts = in_video_pts;
                    }

                    if (video_filter_.video_source) {
                        FF(av_buffersrc_write_frame(video_filter_.video_source, src.get()));
                    }
                    if (audio_filter_.video_source) {
                        FF(av_buffersrc_write_frame(audio_filter_.video_source, src.get()));
                    }
                }

                {
                    IDeckLinkTimecode* tc;
                    if (SUCCEEDED(video->GetTimecode(bmdTimecodeRP188Any, &tc)) && tc) {
                        uint8_t hours, minutes, seconds, frames;
                        if (SUCCEEDED(tc->GetComponents(&hours, &minutes, &seconds, &frames))) {
                            const uint8_t fps = static_cast<uint8_t>(ceil(format_desc_.fps));
                            if (core::frame_timecode::create(
                                    hours, minutes, seconds, frames, fps, true, new_timecode)) {
                                state_["file/timecode"] = new_timecode.string(true);
                            }
                        }

                        tc->Release();
                    }
                }
            }

            if (audio) {
                auto src      = std::shared_ptr<AVFrame>(av_frame_alloc(), [](AVFrame* ptr) { av_frame_free(&ptr); });
                src->format   = AV_SAMPLE_FMT_S32;
                src->channels = format_desc_.audio_channels;
                src->sample_rate = format_desc_.audio_sample_rate;

                void* audio_bytes = nullptr;
                if (SUCCEEDED(audio->GetBytes(&audio_bytes)) && audio_bytes) {
                    audio->AddRef();
                    src = std::shared_ptr<AVFrame>(src.get(), [src, audio](AVFrame* ptr) { audio->Release(); });
                    src->nb_samples  = audio->GetSampleFrameCount();
                    src->data[0]     = reinterpret_cast<uint8_t*>(audio_bytes);
                    src->linesize[0] = src->nb_samples * src->channels *
                                       av_get_bytes_per_sample(static_cast<AVSampleFormat>(src->format));

                    if (SUCCEEDED(audio->GetPacketTime(&in_audio_pts, format_desc_.audio_sample_rate))) {
                        src->pts = in_audio_pts;
                    }

                    if (video_filter_.audio_source) {
                        FF(av_buffersrc_write_frame(video_filter_.audio_source, src.get()));
                    }
                    if (audio_filter_.audio_source) {
                        FF(av_buffersrc_write_frame(audio_filter_.audio_source, src.get()));
                    }
                }
            }

            if (new_timecode != core::frame_timecode::empty())
            {
                // Account for latency introduced by ffmpeg filter
                new_timecode = new_timecode - (format_desc_.field_count > 1 ? 3 : 1);
            }

            while (true) {
                {
                    auto av_video = alloc_frame();
                    auto av_audio = alloc_frame();

                    // TODO (fix) this may get stuck if the decklink sends a frame of video or audio

                    if (av_buffersink_get_frame_flags(video_filter_.sink, av_video.get(), AV_BUFFERSINK_FLAG_PEEK) <
                        0) {
                        return S_OK;
                    }

                    audio_filter_.sink->inputs[0]->min_samples = audio_cadence_[0];
                    if (av_buffersink_get_frame_flags(audio_filter_.sink, av_audio.get(), AV_BUFFERSINK_FLAG_PEEK) <
                        0) {
                        return S_OK;
                    }
                }
                auto av_video = alloc_frame();
                auto av_audio = alloc_frame();

                // TODO (fix) auto V/A sync even if decklink is wrong.

                av_buffersink_get_frame(video_filter_.sink, av_video.get());
                av_buffersink_get_samples(audio_filter_.sink, av_audio.get(), audio_cadence_[0]);

                auto video_tb = av_buffersink_get_time_base(video_filter_.sink);
                auto audio_tb = av_buffersink_get_time_base(audio_filter_.sink);

                auto in_sync = static_cast<double>(in_video_pts) / AV_TIME_BASE -
                               static_cast<double>(in_audio_pts) / format_desc_.audio_sample_rate;
                auto out_sync = static_cast<double>(av_video->pts * video_tb.num) / video_tb.den -
                                static_cast<double>(av_audio->pts * video_tb.num) / audio_tb.den;

                if (std::abs(in_sync - in_sync_) > 0.01) {
                    CASPAR_LOG(warning) << print() << " in-sync changed: " << in_sync;
                }
                in_sync_ = in_sync;

                if (std::abs(out_sync - out_sync_) > 0.01) {
                    CASPAR_LOG(warning) << print() << " out-sync changed: " << out_sync;
                }
                out_sync_ = out_sync;

                graph_->set_value("in-sync", in_sync * 2.0 + 0.5);
                graph_->set_value("out-sync", out_sync * 2.0 + 0.5);

                auto frame = std::make_pair(new_timecode,
                                            core::draw_frame(make_frame(this, *frame_factory_, av_video, av_audio)));
                if (!frame_buffer_.try_push(frame)) {
                    std::pair<core::frame_timecode, core::draw_frame> dummy;
                    frame_buffer_.try_pop(dummy);
                    frame_buffer_.try_push(frame);
                    graph_->set_tag(diagnostics::tag_severity::WARNING, "dropped-frame");
                }

                boost::range::rotate(audio_cadence_, std::end(audio_cadence_) - 1);
            }
        } catch (...) {
            exception_ = std::current_exception();
            return E_FAIL;
        }

        return S_OK;
    }

    core::draw_frame get_frame()
    {
        if (exception_ != nullptr) {
            std::rethrow_exception(exception_);
        }

        auto frame = last_frame_;

        if (!frame_buffer_.try_pop(frame)) {
            if (freeze_on_lost_)
                frame = std::make_pair(core::frame_timecode::empty(), last_frame_.second);
            graph_->set_tag(diagnostics::tag_severity::WARNING, "late-frame");
        } else {
            last_frame_ = frame;
        }

        graph_->set_value("output-buffer",
                          static_cast<float>(frame_buffer_.size()) / static_cast<float>(frame_buffer_.capacity()));

        return frame.second;
    }

    std::wstring print() const
    {
        return model_name_ + L" [" + boost::lexical_cast<std::wstring>(device_index_) + L"|" + format_desc_.name + L"]";
    }

    boost::rational<int> get_out_framerate() const { return format_desc_.framerate; }

    core::monitor::state state() const
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return state_;
    }

    const core::frame_timecode& timecode() const { return last_frame_.first; }
    bool                        has_timecode() const { return last_frame_.first != core::frame_timecode::empty(); }
};

class decklink_producer_proxy : public core::frame_producer
{
    std::unique_ptr<decklink_producer> producer_;
    const uint32_t                     length_;
    executor                           executor_;

  public:
    explicit decklink_producer_proxy(const core::video_format_desc&              format_desc,
                                     const spl::shared_ptr<core::frame_factory>& frame_factory,
                                     int                                         device_index,
                                     const std::string&                          vfilter,
                                     const std::string&                          afilter,
                                     uint32_t                                    length,
                                     bool                                        freeze_on_lost)
        : executor_(L"decklink_producer[" + boost::lexical_cast<std::wstring>(device_index) + L"]")
        , length_(length)
    {
        auto ctx = core::diagnostics::call_context::for_thread();
        executor_.invoke([=] {
            core::diagnostics::call_context::for_thread() = ctx;
            com_initialize();
            producer_.reset(
                new decklink_producer(format_desc, device_index, frame_factory, vfilter, afilter, freeze_on_lost));
        });
    }

    ~decklink_producer_proxy()
    {
        executor_.invoke([=] {
            producer_.reset();
            com_uninitialize();
        });
    }

    core::monitor::state state() const override { return producer_->state(); }

    // frame_producer

    core::draw_frame receive_impl(int nb_samples) override { return producer_->get_frame(); }

    core::draw_frame first_frame() override { return receive_impl(0); }

    uint32_t nb_frames() const override { return length_; }

    std::wstring print() const override { return producer_->print(); }

    std::wstring name() const override { return L"decklink"; }

    boost::rational<int> get_out_framerate() const { return producer_->get_out_framerate(); }

    const core::frame_timecode& timecode() override { return producer_->timecode(); }
    bool                        has_timecode() override { return producer_->has_timecode(); }
    bool                        provides_timecode() override { return true; }
};

spl::shared_ptr<core::frame_producer> create_producer(const core::frame_producer_dependencies& dependencies,
                                                      const std::vector<std::wstring>&         params)
{
    if (params.empty() || !boost::iequals(params.at(0), "decklink"))
        return core::frame_producer::empty();

    auto device_index = get_param(L"DEVICE", params, -1);
    if (device_index == -1)
        device_index = boost::lexical_cast<int>(params.at(1));

    auto freeze_on_lost = contains_param(L"FREEZE_ON_LOST", params);

    auto filter_str = get_param(L"FILTER", params);
    auto length     = get_param(L"LENGTH", params, std::numeric_limits<uint32_t>::max());

    boost::ireplace_all(filter_str, L"DEINTERLACE_BOB", L"YADIF=1:-1");
    boost::ireplace_all(filter_str, L"DEINTERLACE_LQ", L"SEPARATEFIELDS");
    boost::ireplace_all(filter_str, L"DEINTERLACE", L"YADIF=0:-1");

    auto vfilter = boost::to_lower_copy(get_param(L"VF", params, filter_str));
    auto afilter = boost::to_lower_copy(get_param(L"AF", params, get_param(L"FILTER", params, L"")));

    auto producer = spl::make_shared<decklink_producer_proxy>(dependencies.format_desc,
                                                              dependencies.frame_factory,
                                                              device_index,
                                                              u8(vfilter),
                                                              u8(afilter),
                                                              length,
                                                              freeze_on_lost);
    return core::create_destroy_proxy(producer);
}
}} // namespace caspar::decklink
