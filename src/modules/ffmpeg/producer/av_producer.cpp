#include "av_producer.h"

#include "av_input.h"

#include "../util/av_assert.h"
#include "../util/av_util.h"

#include <boost/exception/exception.hpp>
#include <boost/format.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/range/algorithm/rotate.hpp>
#include <boost/rational.hpp>
#include <boost/thread.hpp>
#include <boost/thread/condition_variable.hpp>
#include <boost/thread/mutex.hpp>

#include <common/diagnostics/graph.h>
#include <common/env.h>
#include <common/except.h>
#include <common/executor.h>
#include <common/os/thread.h>
#include <common/scope_exit.h>
#include <common/timer.h>

#include <core/frame/draw_frame.h>
#include <core/frame/frame_factory.h>
#include <core/monitor/monitor.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
}

#include <algorithm>
#include <atomic>
#include <deque>
#include <iomanip>
#include <memory>
#include <queue>
#include <sstream>
#include <string>
#include <thread>

namespace caspar { namespace ffmpeg {

const AVRational TIME_BASE_Q = {1, AV_TIME_BASE};

struct Frame
{
    std::shared_ptr<AVFrame> video;
    std::shared_ptr<AVFrame> audio;
    core::draw_frame         frame;
    int64_t                  start_time  = AV_NOPTS_VALUE;
    int64_t                  pts         = AV_NOPTS_VALUE;
    int64_t                  duration    = 0;
    int64_t                  frame_count = 0;
};

AVPixelFormat get_pix_fmt_with_alpha(AVPixelFormat fmt)
{
    switch (fmt) {
        case AV_PIX_FMT_YUV420P:
            return AV_PIX_FMT_YUVA420P;
        case AV_PIX_FMT_YUV422P:
            return AV_PIX_FMT_YUVA422P;
        case AV_PIX_FMT_YUV444P:
            return AV_PIX_FMT_YUVA444P;
        default:
            break;
    }
    return fmt;
}

const AVCodec* get_decoder(AVCodecID codec_id)
{
    // enforce use of libvpx for vp8 and vp9 codecs to be able
    // to decode webm files with alpha channel
    const AVCodec* result = nullptr;
    if (codec_id == AV_CODEC_ID_VP9)
        result = avcodec_find_decoder_by_name("libvpx-vp9");
    else if (codec_id == AV_CODEC_ID_VP8)
        result = avcodec_find_decoder_by_name("libvpx");
    return result != nullptr ? result : avcodec_find_decoder(codec_id);
}

// TODO (fix) Handle ts discontinuities.
// TODO (feat) Forward options.

class Decoder
{
    Decoder(const Decoder&)            = delete;
    Decoder& operator=(const Decoder&) = delete;

    AVStream*         st       = nullptr;
    int64_t           next_pts = AV_NOPTS_VALUE;
    std::atomic<bool> eof      = {false};

    std::queue<std::shared_ptr<AVPacket>> input;
    mutable boost::mutex                  input_mutex;
    boost::condition_variable             input_cond;
    int                                   input_capacity = 2;

    std::queue<std::shared_ptr<AVFrame>> output;
    mutable boost::mutex                 output_mutex;
    boost::condition_variable            output_cond;
    int                                  output_capacity = 8;

    // Flush-in-place synchronisation. A flush request is raised by the producer thread
    // (Decoder::flush) but is always serviced on the worker thread (do_flush), because the
    // worker is the only thread allowed to touch `ctx`. This lets a looping producer reuse
    // the decoder across a loop wrap instead of destroying and recreating the thread + codec
    // context every loop, which was the source of the heap-allocation churn.
    std::atomic<bool>         flush_request_ = {false};
    mutable boost::mutex      flush_ack_mutex;
    boost::condition_variable flush_ack_cond;

    boost::thread thread;

    // Runs on the worker thread only.
    void do_flush()
    {
        {
            boost::lock_guard<boost::mutex>       lock(input_mutex);
            std::queue<std::shared_ptr<AVPacket>> empty;
            input.swap(empty);
        }
        {
            boost::lock_guard<boost::mutex>      lock(output_mutex);
            std::queue<std::shared_ptr<AVFrame>> empty;
            output.swap(empty);
        }

        avcodec_flush_buffers(ctx.get());
        next_pts = AV_NOPTS_VALUE;
        eof      = false;

        {
            boost::lock_guard<boost::mutex> lock(flush_ack_mutex);
            flush_request_.store(false);
        }
        flush_ack_cond.notify_all();
    }

  public:
    std::shared_ptr<AVCodecContext> ctx;

    Decoder() = default;

    explicit Decoder(AVStream* stream)
        : st(stream)
    {
        const auto codec = get_decoder(stream->codecpar->codec_id);

        if (!codec) {
            FF_RET(AVERROR_DECODER_NOT_FOUND, "avcodec_find_decoder");
        }

        ctx = std::shared_ptr<AVCodecContext>(avcodec_alloc_context3(codec),
                                              [](AVCodecContext* ptr) { avcodec_free_context(&ptr); });

        if (!ctx) {
            FF_RET(AVERROR(ENOMEM), "avcodec_alloc_context3");
        }

        FF(avcodec_parameters_to_context(ctx.get(), stream->codecpar));

        if (stream->metadata != NULL) {
            auto entry = av_dict_get(stream->metadata, "alpha_mode", NULL, AV_DICT_MATCH_CASE);
            if (entry != NULL && entry->value != NULL && *entry->value == '1')
                ctx->pix_fmt = get_pix_fmt_with_alpha(ctx->pix_fmt);
        }

        int thread_count = env::properties().get(L"configuration.ffmpeg.producer.threads", 0);
        FF(av_opt_set_int(ctx.get(), "threads", thread_count, 0));

        ctx->pkt_timebase = stream->time_base;

        if (ctx->codec_type == AVMEDIA_TYPE_VIDEO) {
            ctx->framerate           = av_guess_frame_rate(nullptr, stream, nullptr);
            ctx->sample_aspect_ratio = av_guess_sample_aspect_ratio(nullptr, stream, nullptr);
        } else if (ctx->codec_type == AVMEDIA_TYPE_AUDIO) {
        }

        FF(avcodec_open2(ctx.get(), codec, nullptr));

        thread = boost::thread([this]() {
            try {
                // NOTE: Check interruption via the current-thread helper rather than reading the
                // `thread` member, which is still being assigned by the constructor when this
                // lambda starts running (data race on the partially-constructed member).
                while (!boost::this_thread::interruption_requested()) {
                    // Flush-in-place sync point: service a pending flush on this worker thread
                    // (the only thread allowed to touch ctx) before doing any more decoding.
                    if (flush_request_.load()) {
                        do_flush();
                        continue;
                    }

                    auto av_frame = alloc_frame();
                    auto ret      = avcodec_receive_frame(ctx.get(), av_frame.get());

                    if (ret == AVERROR(EAGAIN)) {
                        std::shared_ptr<AVPacket> packet;
                        {
                            boost::unique_lock<boost::mutex> lock(input_mutex);
                            input_cond.wait(lock, [&]() { return !input.empty() || flush_request_.load(); });
                            if (flush_request_.load()) {
                                continue;
                            }
                            packet = std::move(input.front());
                            input.pop();
                        }
                        FF(avcodec_send_packet(ctx.get(), packet.get()));
                    } else if (ret == AVERROR_EOF) {
                        avcodec_flush_buffers(ctx.get());
                        av_frame->pts = next_pts;
                        next_pts      = AV_NOPTS_VALUE;
                        eof           = true;

                        {
                            boost::unique_lock<boost::mutex> lock(output_mutex);
                            output_cond.wait(
                                lock, [&]() { return output.size() < output_capacity || flush_request_.load(); });
                            if (flush_request_.load()) {
                                continue;
                            }
                            output.push(std::move(av_frame));
                        }
                    } else {
                        FF_RET(ret, "avcodec_receive_frame");

                        // TODO: Maybe Fixed in:
                        // https://github.com/FFmpeg/FFmpeg/commit/33203a08e0a26598cb103508327a1dc184b27bc6
                        // NOTE This is a workaround for DVCPRO HD.
#if LIBAVCODEC_VERSION_MAJOR < 61
                        if (av_frame->width > 1024 && av_frame->interlaced_frame) {
                            av_frame->top_field_first = 1;
                        }
#else
                        if (av_frame->width > 1024 && (av_frame->flags & AV_FRAME_FLAG_INTERLACED)) {
                            av_frame->flags |= AV_FRAME_FLAG_TOP_FIELD_FIRST;
                        }
#endif

                        // TODO (fix) is this always best?
                        av_frame->pts = av_frame->best_effort_timestamp;

                        auto duration_pts = av_frame->duration;
                        if (duration_pts <= 0) {
                            if (ctx->codec_type == AVMEDIA_TYPE_VIDEO) {
#if LIBAVCODEC_VERSION_MAJOR < 62
                                const int ticks_per_frame = ctx->ticks_per_frame;
#else
                                // https://github.com/FFmpeg/FFmpeg/commit/e930b834a928546f9cbc937f6633709053448232#diff-115616f8a2b59cab3aac4e7f4c8c31e69e94e7fcfa339b9f65b0bf34308aa80fR682
                                const int ticks_per_frame =
                                    (ctx->codec_descriptor && (ctx->codec_descriptor->props & AV_CODEC_PROP_FIELDS))
                                        ? 2
                                        : 1;
#endif
                                const auto ticks = av_stream_get_parser(st) ? av_stream_get_parser(st)->repeat_pict + 1
                                                                            : ticks_per_frame;
                                duration_pts     = static_cast<int64_t>(AV_TIME_BASE) * ctx->framerate.den * ticks /
                                               ctx->framerate.num / ticks_per_frame;
                                duration_pts = av_rescale_q(duration_pts, {1, AV_TIME_BASE}, st->time_base);
                            } else if (ctx->codec_type == AVMEDIA_TYPE_AUDIO) {
                                duration_pts = av_rescale_q(av_frame->nb_samples, {1, ctx->sample_rate}, st->time_base);
                            }
                        }

                        if (duration_pts > 0) {
                            next_pts = av_frame->pts + duration_pts;
                        } else {
                            next_pts = AV_NOPTS_VALUE;
                        }

                        {
                            boost::unique_lock<boost::mutex> lock(output_mutex);
                            output_cond.wait(
                                lock, [&]() { return output.size() < output_capacity || flush_request_.load(); });
                            if (flush_request_.load()) {
                                continue;
                            }
                            output.push(std::move(av_frame));
                        }
                    }
                }
            } catch (boost::thread_interrupted&) {
                // Do nothing...
            } catch (...) {
                eof = true;
                CASPAR_LOG_CURRENT_EXCEPTION();
            }
        });
    }

    ~Decoder()
    {
        try {
            if (thread.joinable()) {
                thread.interrupt();
                thread.join();
            }
        } catch (boost::thread_interrupted&) {
            // Do nothing...
        }
    }

    bool want_packet() const
    {
        if (eof) {
            return false;
        }

        {
            boost::lock_guard<boost::mutex> lock(input_mutex);
            return input.size() < input_capacity;
        }
    }

    void push(std::shared_ptr<AVPacket> packet)
    {
        if (eof) {
            return;
        }

        {
            boost::lock_guard<boost::mutex> lock(input_mutex);
            input.push(std::move(packet));
        }

        input_cond.notify_all();
    }

    std::shared_ptr<AVFrame> pop()
    {
        std::shared_ptr<AVFrame> frame;

        {
            boost::lock_guard<boost::mutex> lock(output_mutex);

            if (!output.empty()) {
                frame = std::move(output.front());
                output.pop();
            }
        }

        if (frame) {
            output_cond.notify_all();
        } else if (eof) {
            frame = alloc_frame();
        }

        return frame;
    }

    // Reset the decoder to a fresh, post-seek state (empty queues, flushed codec buffers)
    // without destroying the worker thread or codec context. Called by the producer thread;
    // blocks until the worker thread has performed the flush at a safe sync point.
    void flush()
    {
        if (!thread.joinable()) {
            return;
        }

        flush_request_.store(true);
        input_cond.notify_all();
        output_cond.notify_all();

        boost::unique_lock<boost::mutex> lock(flush_ack_mutex);
        flush_ack_cond.wait(lock, [&]() { return !flush_request_.load(); });
    }
};

struct Filter
{
    std::shared_ptr<AVFilterGraph>  graph;
    AVFilterContext*                sink = nullptr;
    std::map<int, AVFilterContext*> sources;
    std::shared_ptr<AVFrame>        frame;
    bool                            eof = false;

    Filter() = default;

    Filter(std::string                    filter_spec,
           const Input&                   input,
           std::map<int, Decoder>&        streams,
           int64_t                        start_time,
           AVMediaType                    media_type,
           const core::video_format_desc& format_desc)
    {
        if (media_type == AVMEDIA_TYPE_VIDEO) {
            if (filter_spec.empty()) {
                filter_spec = "null";
            }

            auto deint = u8(
                env::properties().get<std::wstring>(L"configuration.ffmpeg.producer.auto-deinterlace", L"interlaced"));

            if (deint != "none") {
                filter_spec += (boost::format(",bwdif=mode=send_field:parity=auto:deint=%s") % deint).str();
            }

            filter_spec += (boost::format(",fps=fps=%d/%d:start_time=%f") %
                            (format_desc.framerate.numerator() * format_desc.field_count) %
                            format_desc.framerate.denominator() % (static_cast<double>(start_time) / AV_TIME_BASE))
                               .str();
        } else if (media_type == AVMEDIA_TYPE_AUDIO) {
            if (filter_spec.empty()) {
                filter_spec = "anull";
            }

            // Find first audio stream to get a time_base for the first_pts calculation
            AVRational tb = {1, format_desc.audio_sample_rate};
            for (auto n = 0U; n < input->nb_streams; ++n) {
                const auto st             = input->streams[n];
                const auto codec_channels = st->codecpar->ch_layout.nb_channels;
                if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && codec_channels > 0) {
                    tb = {1, st->codecpar->sample_rate};
                    break;
                }
            }
            filter_spec += (boost::format(",aresample=async=1000:first_pts=%d:min_comp=0.01:osr=%d,"
                                          "asetnsamples=n=1024:p=0") %
                            av_rescale_q(start_time, TIME_BASE_Q, tb) % format_desc.audio_sample_rate)
                               .str();
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
                const auto type = avfilter_pad_get_type(cur->filter_ctx->input_pads, cur->pad_idx);
                if (type == AVMEDIA_TYPE_VIDEO) {
                    video_input_count += 1;
                } else if (type == AVMEDIA_TYPE_AUDIO) {
                    audio_input_count += 1;
                }
            }
        }

        std::deque<AVStream*> video_streams, audio_streams;
        for (auto n = 0U; n < input->nb_streams; ++n) {
            const auto st = input->streams[n];

            const auto codec_channels = st->codecpar->ch_layout.nb_channels;

            auto disposition = st->disposition;
            if (!disposition || disposition == AV_DISPOSITION_DEFAULT) {
                switch (st->codecpar->codec_type) {
                    case AVMEDIA_TYPE_VIDEO:
                        video_streams.push_back(st);
                        break;
                    case AVMEDIA_TYPE_AUDIO:
                        if (codec_channels > 0) {
                            audio_streams.push_back(st);
                        }
                        break;
                    default:
                        // ignore stream
                        break;
                }
            }
        }

        if (audio_input_count == 1) {
            // TODO (fix) Use some form of stream meta data to do this.
            // https://github.com/CasparCG/server/issues/833
            if (audio_streams.size() > 1) {
                filter_spec = (boost::format("amerge=inputs=%d,") % audio_streams.size()).str() + filter_spec;
            }
        }

        if (video_input_count == 1) {
            std::stable_sort(video_streams.begin(), video_streams.end(), [&](AVStream* lhs, AVStream* rhs) {
                return lhs->codecpar->height > rhs->codecpar->height;
            });

            auto same_properties = [](AVStream* lhs, AVStream* rhs) {
                auto lcp = lhs->codecpar;
                auto rcp = rhs->codecpar;
                return lcp->width == rcp->width && lcp->height == rcp->height &&
                       lcp->sample_aspect_ratio.num == rcp->sample_aspect_ratio.num &&
                       lcp->sample_aspect_ratio.den == rcp->sample_aspect_ratio.den &&
                       lcp->field_order == rcp->field_order;
            };

            // TODO (fix) Use some form of stream meta data to do this.
            // https://github.com/CasparCG/server/issues/832
            if (video_streams.size() > 1 && same_properties(video_streams[0], video_streams[1])) {
                if (video_streams.size() == 2 || !same_properties(video_streams[0], video_streams[2])) {
                    filter_spec = "alphamerge," + filter_spec;
                }
            }
        }

        graph = std::shared_ptr<AVFilterGraph>(avfilter_graph_alloc(),
                                               [](AVFilterGraph* ptr) { avfilter_graph_free(&ptr); });

        if (!graph) {
            FF_RET(AVERROR(ENOMEM), "avfilter_graph_alloc");
        }

        FF(avfilter_graph_parse2(graph.get(), filter_spec.c_str(), &inputs, &outputs));

        // inputs
        {
            for (auto cur = inputs; cur; cur = cur->next) {
                const auto type = avfilter_pad_get_type(cur->filter_ctx->input_pads, cur->pad_idx);

                AVStream* stream;

                switch (type) {
                    case AVMEDIA_TYPE_VIDEO:
                        if (video_streams.empty()) {
                            graph = nullptr;
                            return;
                        }
                        // TODO find stream based on link name
                        stream = video_streams.front();
                        video_streams.pop_front();
                        break;
                    case AVMEDIA_TYPE_AUDIO:
                        if (audio_streams.empty()) {
                            graph = nullptr;
                            return;
                        }
                        // TODO find stream based on link name
                        stream = audio_streams.front();
                        audio_streams.pop_front();
                        break;
                    default:
                        CASPAR_THROW_EXCEPTION(ffmpeg_error_t()
                                               << boost::errinfo_errno(EINVAL)
                                               << msg_info_t("only video and audio filters supported"));
                }

                auto it = streams.find(stream->index);
                if (it == streams.end()) {
                    it = streams.emplace(stream->index, stream).first;
                }

                auto st = it->second.ctx;

                if (st->codec_type == AVMEDIA_TYPE_VIDEO) {
                    auto args = (boost::format("video_size=%dx%d:pix_fmt=%d:time_base=%d/%d") % st->width % st->height %
                                 st->pix_fmt % st->pkt_timebase.num % st->pkt_timebase.den)
                                    .str();
                    auto name = (boost::format("in_%d") % stream->index).str();

                    if (st->sample_aspect_ratio.num > 0 && st->sample_aspect_ratio.den > 0) {
                        args +=
                            (boost::format(":sar=%d/%d") % st->sample_aspect_ratio.num % st->sample_aspect_ratio.den)
                                .str();
                    }

                    if (st->framerate.num > 0 && st->framerate.den > 0) {
                        args += (boost::format(":frame_rate=%d/%d") % st->framerate.num % st->framerate.den).str();
                    }

                    AVFilterContext* source = nullptr;
                    FF(avfilter_graph_create_filter(
                        &source, avfilter_get_by_name("buffer"), name.c_str(), args.c_str(), nullptr, graph.get()));
                    FF(avfilter_link(source, 0, cur->filter_ctx, cur->pad_idx));
                    sources.emplace(stream->index, source);
                } else if (st->codec_type == AVMEDIA_TYPE_AUDIO) {
                    char channel_layout[128];
                    FF(av_channel_layout_describe(&st->ch_layout, channel_layout, sizeof(channel_layout)));

                    auto args = (boost::format("time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=%#x") %
                                 st->pkt_timebase.num % st->pkt_timebase.den % st->sample_rate %
                                 av_get_sample_fmt_name(st->sample_fmt) % channel_layout)
                                    .str();
                    auto name = (boost::format("in_%d") % stream->index).str();

                    AVFilterContext* source = nullptr;
                    FF(avfilter_graph_create_filter(
                        &source, avfilter_get_by_name("abuffer"), name.c_str(), args.c_str(), nullptr, graph.get()));
                    FF(avfilter_link(source, 0, cur->filter_ctx, cur->pad_idx));
                    sources.emplace(stream->index, source);
                } else {
                    CASPAR_THROW_EXCEPTION(ffmpeg_error_t() << boost::errinfo_errno(EINVAL)
                                                            << msg_info_t("invalid filter input media type"));
                }
            }
        }

        if (media_type == AVMEDIA_TYPE_VIDEO) {
            sink = FFMEM(avfilter_graph_alloc_filter(graph.get(), avfilter_get_by_name("buffersink"), "out"));

            const AVPixelFormat pix_fmts[] = {AV_PIX_FMT_RGB24,
                                              AV_PIX_FMT_BGR24,
                                              AV_PIX_FMT_BGRA,
                                              AV_PIX_FMT_ARGB,
                                              AV_PIX_FMT_RGBA,
                                              AV_PIX_FMT_ABGR,
                                              AV_PIX_FMT_YUV444P,
                                              AV_PIX_FMT_YUV444P10,
                                              AV_PIX_FMT_YUV444P12,
                                              AV_PIX_FMT_YUV422P,
                                              AV_PIX_FMT_YUV422P10,
                                              AV_PIX_FMT_YUV422P12,
                                              AV_PIX_FMT_YUV420P,
                                              AV_PIX_FMT_YUV420P10,
                                              AV_PIX_FMT_YUV420P12,
                                              AV_PIX_FMT_YUV410P,
                                              AV_PIX_FMT_YUVA444P,
                                              AV_PIX_FMT_YUVA422P,
                                              AV_PIX_FMT_YUVA420P,
                                              AV_PIX_FMT_UYVY422,
                                              // bwdif needs planar rgb
                                              AV_PIX_FMT_GBRP,
                                              AV_PIX_FMT_GBRP10,
                                              AV_PIX_FMT_GBRP12,
                                              AV_PIX_FMT_GBRP16,
                                              AV_PIX_FMT_GBRAP,
                                              AV_PIX_FMT_GBRAP16,
                                              AV_PIX_FMT_NONE};
#if LIBAVUTIL_VERSION_MAJOR >= 60 // FFmpeg 8
            FF(av_opt_set_array(sink,
                                "pixel_formats",
                                AV_OPT_SEARCH_CHILDREN | AV_OPT_ARRAY_REPLACE,
                                0,
                                FF_ARRAY_ELEMS(pix_fmts) - 1,
                                AV_OPT_TYPE_PIXEL_FMT,
                                pix_fmts));
#else
            FF(av_opt_set_int_list(sink, "pix_fmts", pix_fmts, -1, AV_OPT_SEARCH_CHILDREN));
#endif
        } else if (media_type == AVMEDIA_TYPE_AUDIO) {
            sink = FFMEM(avfilter_graph_alloc_filter(graph.get(), avfilter_get_by_name("abuffersink"), "out"));

            const AVSampleFormat sample_fmts[]  = {AV_SAMPLE_FMT_S32, AV_SAMPLE_FMT_NONE};
            const int            sample_rates[] = {format_desc.audio_sample_rate, -1};

            FF(av_opt_set_int(sink, "all_channel_counts", 1, AV_OPT_SEARCH_CHILDREN));

#if LIBAVUTIL_VERSION_MAJOR >= 60 // FFmpeg 8
            FF(av_opt_set_array(sink,
                                "sample_formats",
                                AV_OPT_SEARCH_CHILDREN | AV_OPT_ARRAY_REPLACE,
                                0,
                                FF_ARRAY_ELEMS(sample_fmts) - 1,
                                AV_OPT_TYPE_SAMPLE_FMT,
                                sample_fmts));
            FF(av_opt_set_array(sink,
                                "samplerates",
                                AV_OPT_SEARCH_CHILDREN | AV_OPT_ARRAY_REPLACE,
                                0,
                                FF_ARRAY_ELEMS(sample_rates) - 1,
                                AV_OPT_TYPE_INT,
                                sample_rates));
#else
            FF(av_opt_set_int_list(sink, "sample_fmts", sample_fmts, -1, AV_OPT_SEARCH_CHILDREN));
            FF(av_opt_set_int_list(sink, "sample_rates", sample_rates, -1, AV_OPT_SEARCH_CHILDREN));
#endif
        } else {
            CASPAR_THROW_EXCEPTION(ffmpeg_error_t()
                                   << boost::errinfo_errno(EINVAL) << msg_info_t("invalid output media type"));
        }

        FF(avfilter_init_str(sink, nullptr));

        // output
        {
            const auto cur = outputs;

            if (!cur || cur->next) {
                CASPAR_THROW_EXCEPTION(ffmpeg_error_t() << boost::errinfo_errno(EINVAL)
                                                        << msg_info_t("invalid filter graph output count"));
            }

            if (avfilter_pad_get_type(cur->filter_ctx->output_pads, cur->pad_idx) != media_type) {
                CASPAR_THROW_EXCEPTION(ffmpeg_error_t() << boost::errinfo_errno(EINVAL)
                                                        << msg_info_t("invalid filter output media type"));
            }

            FF(avfilter_link(cur->filter_ctx, cur->pad_idx, sink, 0));
        }

        FF(avfilter_graph_config(graph.get(), nullptr));

        CASPAR_LOG(debug) << avfilter_graph_dump(graph.get(), nullptr);
    }

    bool operator()(int nb_samples = -1)
    {
        if (frame || eof) {
            return false;
        }

        if (!sink || sources.empty()) {
            eof   = true;
            frame = nullptr;
            return true;
        }

        auto av_frame = alloc_frame();
        auto ret      = nb_samples >= 0 ? av_buffersink_get_samples(sink, av_frame.get(), nb_samples)
                                        : av_buffersink_get_frame(sink, av_frame.get());

        if (ret == AVERROR(EAGAIN)) {
            return false;
        }
        if (ret == AVERROR_EOF) {
            eof   = true;
            frame = nullptr;
            return true;
        }
        FF_RET(ret, "av_buffersink_get_frame");
        frame = std::move(av_frame);
        return true;
    }
};

struct AVProducer::Impl
{
    caspar::core::monitor::state state_;
    mutable boost::mutex         state_mutex_;

    spl::shared_ptr<diagnostics::graph> graph_;

    const std::shared_ptr<core::frame_factory> frame_factory_;
    const core::video_format_desc              format_desc_;
    const AVRational                           format_tb_;
    const std::string                          name_;
    const std::string                          path_;

    Input                  input_;
    std::map<int, Decoder> decoders_;
    Filter                 video_filter_;
    Filter                 audio_filter_;

    std::map<int, std::vector<AVFilterContext*>> sources_;

    std::atomic<int64_t> start_{AV_NOPTS_VALUE};
    std::atomic<int64_t> duration_{AV_NOPTS_VALUE};
    std::atomic<int64_t> input_duration_{AV_NOPTS_VALUE};
    std::atomic<int64_t> seek_{AV_NOPTS_VALUE};
    std::atomic<bool>    loop_{false};

    std::string afilter_;
    std::string vfilter_;

    int                              seekable_ = 2;
    core::frame_geometry::scale_mode scale_mode_;
    int64_t                          frame_count_    = 0;
    bool                             frame_flush_    = true;
    int64_t                          frame_time_     = AV_NOPTS_VALUE;
    int64_t                          frame_duration_ = AV_NOPTS_VALUE;
    core::draw_frame                 frame_;

    std::deque<Frame>         buffer_;
    mutable boost::mutex      buffer_mutex_;
    boost::condition_variable buffer_cond_;
    std::atomic<bool>         buffer_eof_{false};
    int                       buffer_capacity_ = static_cast<int>(format_desc_.fps) / 4;

    std::optional<caspar::executor> video_executor_;
    std::optional<caspar::executor> audio_executor_;

    int latency_ = 0;

    boost::thread thread_;

    Impl(std::shared_ptr<core::frame_factory> frame_factory,
         core::video_format_desc              format_desc,
         std::string                          name,
         std::string                          path,
         std::string                          vfilter,
         std::string                          afilter,
         std::optional<int64_t>               start,
         std::optional<int64_t>               seek,
         std::optional<int64_t>               duration,
         bool                                 loop,
         int                                  seekable,
         core::frame_geometry::scale_mode     scale_mode)
        : frame_factory_(frame_factory)
        , format_desc_(format_desc)
        , format_tb_({format_desc.duration, format_desc.time_scale * format_desc.field_count})
        , name_(name)
        , path_(path)
        , input_(path, graph_, seekable >= 0 && seekable < 2 ? std::optional<bool>(false) : std::optional<bool>())
        , start_(start ? av_rescale_q(*start, format_tb_, TIME_BASE_Q) : AV_NOPTS_VALUE)
        , duration_(duration ? av_rescale_q(*duration, format_tb_, TIME_BASE_Q) : AV_NOPTS_VALUE)
        , loop_(loop)
        , afilter_(afilter)
        , vfilter_(vfilter)
        , seekable_(seekable)
        , scale_mode_(scale_mode)
        , video_executor_(L"video-executor")
        , audio_executor_(L"audio-executor")
    {
        diagnostics::register_graph(graph_);
        graph_->set_color("underflow", diagnostics::color(0.6f, 0.3f, 0.9f));
        graph_->set_color("frame-time", diagnostics::color(0.0f, 1.0f, 0.0f));
        graph_->set_color("decode-time", diagnostics::color(0.0f, 1.0f, 1.0f));
        graph_->set_color("buffer", diagnostics::color(1.0f, 1.0f, 0.0f));

        state_["file/name"] = u8(name_);
        state_["file/path"] = u8(path_);
        state_["loop"]      = loop;
        update_state();

        CASPAR_LOG(debug) << print() << " seekable: " << seekable_;

        thread_ = boost::thread([=, this] {
            try {
                run(seek);
            } catch (boost::thread_interrupted&) {
                // Do nothing...
            } catch (ffmpeg::ffmpeg_error_t& ex) {
                if (auto errn = boost::get_error_info<ffmpeg_errn_info>(ex)) {
                    if (*errn == AVERROR_EXIT) {
                        return;
                    }
                }
                CASPAR_LOG_CURRENT_EXCEPTION();
            } catch (...) {
                CASPAR_LOG_CURRENT_EXCEPTION();
            }
        });
    }

    ~Impl()
    {
        // 1) Stop feeding the pipeline.
        input_.abort();

        // 2) Join the run loop FIRST, so nothing else touches decoders_/filters while we
        //    tear them down.
        try {
            if (thread_.joinable()) {
                thread_.interrupt();
                thread_.join();
            }
        } catch (boost::thread_interrupted&) {
            // Do nothing...
        }

        // 3) Stop the filter executors before freeing the graphs they reference.
        video_executor_.reset();
        audio_executor_.reset();

        // 4) Destroy the pipeline explicitly, in a defined order, while fully quiesced,
        //    instead of relying on implicit member-destruction order.
        sources_.clear();
        video_filter_ = Filter{};
        audio_filter_ = Filter{};
        decoders_.clear(); // joins each Decoder's worker thread

        CASPAR_LOG(debug) << print() << " Joined";
    }

    void run(std::optional<int64_t> firstSeek)
    {
        std::vector<int> audio_cadence = format_desc_.audio_cadence;

        input_.reset();
        {
            core::monitor::state streams;
            for (auto n = 0UL; n < input_->nb_streams; ++n) {
                auto st                             = input_->streams[n];
                auto framerate                      = av_guess_frame_rate(nullptr, st, nullptr);
                streams[std::to_string(n) + "/fps"] = {framerate.num, framerate.den};
            }

            boost::lock_guard<boost::mutex> lock(state_mutex_);
            state_["file/streams"] = streams;
        }

        if (input_duration_ == AV_NOPTS_VALUE) {
            input_duration_ = input_->duration;
        }

        {
            const auto start = start_.load();
            if (duration_ == AV_NOPTS_VALUE && input_->duration > 0) {
                if (start != AV_NOPTS_VALUE) {
                    duration_ = input_->duration - start;
                } else {
                    duration_ = input_->duration;
                }
            }

            const auto firstStart = firstSeek ? av_rescale_q(*firstSeek, format_tb_, TIME_BASE_Q) : start;
            if (firstStart != AV_NOPTS_VALUE) {
                seek_internal(firstStart);
            } else {
                reset(input_->start_time != AV_NOPTS_VALUE ? input_->start_time : 0);
            }
        }

        set_thread_name(L"[ffmpeg::av_producer]");

        boost::range::rotate(audio_cadence, std::end(audio_cadence) - 1);

        Frame frame;
        timer frame_timer;
        timer decode_timer;

        int warning_debounce = 0;

        while (!thread_.interruption_requested()) {
            {
                const auto seek = seek_.exchange(AV_NOPTS_VALUE);

                if (seek != AV_NOPTS_VALUE) {
                    seek_internal(seek);
                    frame = Frame{};
                    continue;
                }
            }

            {
                // TODO (perf) seek as soon as input is past duration or eof.

                auto start    = start_.load();
                auto duration = duration_.load();

                start       = start != AV_NOPTS_VALUE ? start : 0;
                auto end    = duration != AV_NOPTS_VALUE ? start + duration : INT64_MAX;
                auto time   = frame.pts != AV_NOPTS_VALUE ? frame.pts + frame.duration : 0;
                buffer_eof_ = (video_filter_.eof && audio_filter_.eof) ||
                              av_rescale_q(time, TIME_BASE_Q, format_tb_) >= av_rescale_q(end, TIME_BASE_Q, format_tb_);

                if (buffer_eof_) {
                    if (loop_ && frame_count_ > 2) {
                        frame = Frame{};
                        seek_internal(start);
                    } else {
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                    // TODO (fix) Limit live polling due to bugs.
                    continue;
                }
            }

            bool progress = false;
            {
                progress |= schedule();

                std::vector<std::future<bool>> futures;

                if (!video_filter_.frame) {
                    futures.push_back(video_executor_->begin_invoke([&]() { return video_filter_(); }));
                }

                if (!audio_filter_.frame) {
                    futures.push_back(audio_executor_->begin_invoke([&]() { return audio_filter_(audio_cadence[0]); }));
                }

                for (auto& future : futures) {
                    progress |= future.get();
                }
            }

            if ((!video_filter_.frame && !video_filter_.eof) || (!audio_filter_.frame && !audio_filter_.eof)) {
                if (!progress) {
                    if (warning_debounce++ % 500 == 100) {
                        if (!video_filter_.frame && !video_filter_.eof) {
                            CASPAR_LOG(warning) << print() << " Waiting for video frame...";
                        } else if (!audio_filter_.frame && !audio_filter_.eof) {
                            CASPAR_LOG(warning) << print() << " Waiting for audio frame...";
                        } else {
                            CASPAR_LOG(warning) << print() << " Waiting for frame...";
                        }
                    }

                    // TODO (perf): Avoid live loop.
                    std::this_thread::sleep_for(std::chrono::milliseconds(warning_debounce > 25 ? 20 : 5));
                }
                continue;
            }

            warning_debounce = 0;

            // TODO (fix)
            // if (start_ != AV_NOPTS_VALUE && frame.pts < start_) {
            //    seek_internal(start_);
            //    continue;
            //}

            const auto start_time = input_->start_time != AV_NOPTS_VALUE ? input_->start_time : 0;

            if (video_filter_.frame) {
                frame.video      = std::move(video_filter_.frame);
                const auto tb    = av_buffersink_get_time_base(video_filter_.sink);
                const auto fr    = av_buffersink_get_frame_rate(video_filter_.sink);
                frame.start_time = start_time;
                frame.pts        = av_rescale_q(frame.video->pts, tb, TIME_BASE_Q) - start_time;
                frame.duration   = av_rescale_q(1, av_inv_q(fr), TIME_BASE_Q);
            }

            if (audio_filter_.frame) {
                frame.audio      = std::move(audio_filter_.frame);
                const auto tb    = av_buffersink_get_time_base(audio_filter_.sink);
                const auto sr    = av_buffersink_get_sample_rate(audio_filter_.sink);
                frame.start_time = start_time;
                frame.pts        = av_rescale_q(frame.audio->pts, tb, TIME_BASE_Q) - start_time;
                frame.duration   = av_rescale_q(frame.audio->nb_samples, {1, sr}, TIME_BASE_Q);
            }

            frame.frame = core::draw_frame(
                make_frame(this, *frame_factory_, frame.video, frame.audio, get_color_space(frame.video), scale_mode_));
            frame.frame_count = frame_count_++;

            graph_->set_value("decode-time", decode_timer.elapsed() * format_desc_.fps * 0.5);

            {
                boost::unique_lock<boost::mutex> buffer_lock(buffer_mutex_);
                buffer_cond_.wait(buffer_lock, [&] { return buffer_.size() < buffer_capacity_; });
                if (seek_ == AV_NOPTS_VALUE) {
                    buffer_.push_back(frame);
                }
            }

            if (format_desc_.field_count != 2 || frame_count_ % 2 == 1) {
                // Update the frame-time every other frame when interlaced
                graph_->set_value("frame-time", frame_timer.elapsed() * format_desc_.hz * 0.5);
                frame_timer.restart();
            }

            decode_timer.restart();

            graph_->set_value("buffer", static_cast<double>(buffer_.size()) / static_cast<double>(buffer_capacity_));

            boost::range::rotate(audio_cadence, std::end(audio_cadence) - 1);
        }
    }

    void update_state()
    {
        graph_->set_text(u16(print()));
        boost::lock_guard<boost::mutex> lock(state_mutex_);
        state_["file/clip"] = {start().value_or(0) / format_desc_.fps, duration().value_or(0) / format_desc_.fps};
        state_["file/time"] = {time() / format_desc_.fps, file_duration().value_or(0) / format_desc_.fps};
        state_["loop"]      = loop_;
    }

    core::draw_frame prev_frame(const core::video_field field)
    {
        CASPAR_SCOPE_EXIT { update_state(); };

        // Don't start a new frame on the 2nd field
        if (field != core::video_field::b) {
            if (frame_flush_ || !frame_) {
                boost::lock_guard<boost::mutex> lock(buffer_mutex_);

                if (!buffer_.empty()) {
                    frame_          = buffer_[0].frame;
                    frame_time_     = buffer_[0].pts;
                    frame_duration_ = buffer_[0].duration;
                    frame_flush_    = false;
                }
            }
        }

        return core::draw_frame::still(frame_);
    }

    bool is_ready()
    {
        boost::lock_guard<boost::mutex> lock(buffer_mutex_);
        return !buffer_.empty() || frame_;
    }

    core::draw_frame next_frame(const core::video_field field)
    {
        CASPAR_SCOPE_EXIT { update_state(); };

        boost::lock_guard<boost::mutex> lock(buffer_mutex_);

        if (buffer_.empty() || (frame_flush_ && buffer_.size() < 4)) {
            auto start    = start_.load();
            auto duration = duration_.load();

            start    = start != AV_NOPTS_VALUE ? start : 0;
            auto end = duration != AV_NOPTS_VALUE ? start + duration : INT64_MAX;

            if (buffer_eof_ && !frame_flush_) {
                if (frame_time_ < end && frame_duration_ != AV_NOPTS_VALUE) {
                    frame_time_ += frame_duration_;
                } else if (frame_time_ < end) {
                    frame_time_ = input_duration_;
                }
                return core::draw_frame::still(frame_);
            }
            graph_->set_tag(diagnostics::tag_severity::WARNING, "underflow");
            latency_ += 1;
            return core::draw_frame{};
        }

        if (format_desc_.field_count == 2) {
            // Check if the next frame is the correct 'field'
            auto is_field_1 = (buffer_[0].frame_count % 2) == 0;
            if ((field == core::video_field::a && !is_field_1) || (field == core::video_field::b && is_field_1)) {
                graph_->set_tag(diagnostics::tag_severity::WARNING, "underflow");
                latency_ += 1;
                return core::draw_frame{};
            }
        }

        if (latency_ != -1) {
            CASPAR_LOG(warning) << print() << " Latency: " << latency_;
            latency_ = -1;
        }

        frame_          = buffer_[0].frame;
        frame_time_     = buffer_[0].pts;
        frame_duration_ = buffer_[0].duration;
        frame_flush_    = false;

        buffer_.pop_front();
        buffer_cond_.notify_all();

        graph_->set_value("buffer", static_cast<double>(buffer_.size()) / static_cast<double>(buffer_capacity_));

        return frame_;
    }

    void seek(int64_t time)
    {
        CASPAR_SCOPE_EXIT { update_state(); };

        seek_ = av_rescale_q(time, format_tb_, TIME_BASE_Q);

        {
            boost::lock_guard<boost::mutex> lock(buffer_mutex_);
            buffer_.clear();
            buffer_cond_.notify_all();
            graph_->set_value("buffer", static_cast<double>(buffer_.size()) / static_cast<double>(buffer_capacity_));
        }
    }

    int64_t time() const
    {
        if (frame_time_ == AV_NOPTS_VALUE) {
            // TODO (fix) How to handle NOPTS case?
            return 0;
        }

        return av_rescale_q(frame_time_, TIME_BASE_Q, format_tb_);
    }

    void loop(bool loop)
    {
        CASPAR_SCOPE_EXIT { update_state(); };

        loop_ = loop;
    }

    bool loop() const { return loop_; }

    void start(int64_t start)
    {
        CASPAR_SCOPE_EXIT { update_state(); };
        start_ = av_rescale_q(start, format_tb_, TIME_BASE_Q);
    }

    std::optional<int64_t> start() const
    {
        auto start = start_.load();
        return start != AV_NOPTS_VALUE ? av_rescale_q(start, TIME_BASE_Q, format_tb_) : std::optional<int64_t>();
    }

    void duration(int64_t duration)
    {
        CASPAR_SCOPE_EXIT { update_state(); };

        duration_ = av_rescale_q(duration, format_tb_, TIME_BASE_Q);
    }

    std::optional<int64_t> duration() const
    {
        const auto duration = duration_.load();
        if (duration == AV_NOPTS_VALUE) {
            return {};
        }
        return av_rescale_q(duration, TIME_BASE_Q, format_tb_);
    }

    std::optional<int64_t> file_duration() const
    {
        const auto input_duration = input_duration_.load();
        if (input_duration == AV_NOPTS_VALUE) {
            return {};
        }
        return av_rescale_q(input_duration, TIME_BASE_Q, format_tb_);
    }

  private:
    bool want_packet()
    {
        return std::any_of(decoders_.begin(), decoders_.end(), [](auto& p) { return p.second.want_packet(); });
    }

    bool schedule()
    {
        auto result = false;

        std::shared_ptr<AVPacket> packet;
        while (want_packet() && input_.try_pop(packet)) {
            result = true;

            if (!packet) {
                for (auto& p : decoders_) {
                    p.second.push(nullptr);
                }
            } else if (sources_.find(packet->stream_index) != sources_.end()) {
                auto it = decoders_.find(packet->stream_index);
                if (it != decoders_.end()) {
                    // TODO (fix): limit it->second.input.size()?
                    it->second.push(std::move(packet));
                }
            }
        }

        std::vector<int> eof;

        for (auto& p : sources_) {
            auto it = decoders_.find(p.first);
            if (it == decoders_.end()) {
                continue;
            }

            auto nb_requests = 0U;
            for (auto source : p.second) {
                nb_requests = std::max(nb_requests, av_buffersrc_get_nb_failed_requests(source));
            }

            if (nb_requests == 0) {
                continue;
            }

            auto frame = it->second.pop();
            if (!frame) {
                continue;
            }

            for (auto& source : p.second) {
                if (!frame->data[0]) {
                    FF(av_buffersrc_close(source, frame->pts, 0));
                } else {
                    // TODO (fix) Guard against overflow?
                    FF(av_buffersrc_write_frame(source, frame.get()));
                }
                result = true;
            }

            // End Of File
            if (!frame->data[0]) {
                eof.push_back(p.first);
            }
        }

        for (auto index : eof) {
            sources_.erase(index);
        }

        return result;
    }

    void seek_internal(int64_t time)
    {
        time = time != AV_NOPTS_VALUE ? time : 0;
        time = time + (input_->start_time != AV_NOPTS_VALUE ? input_->start_time : 0);

        // TODO (fix) Dont seek if time is close future.
        if (seekable_) {
            input_.seek(time);
        }
        frame_flush_ = true;
        frame_count_ = 0;
        buffer_eof_  = false;

        // Fix 4b: reuse the existing decoders across a loop wrap / seek instead of destroying
        // and recreating their worker threads and codec contexts every time. Flushing in place
        // resets each decoder to a fresh post-seek state while avoiding the per-loop heap churn.
        // reset() below rebuilds the (cheap) filter graphs and re-binds them to these decoders.
        for (auto& p : decoders_) {
            p.second.flush();
        }

        reset(time);
    }

    void reset(int64_t start_time)
    {
        video_filter_ = Filter(vfilter_, input_, decoders_, start_time, AVMEDIA_TYPE_VIDEO, format_desc_);
        audio_filter_ = Filter(afilter_, input_, decoders_, start_time, AVMEDIA_TYPE_AUDIO, format_desc_);

        sources_.clear();
        for (auto& p : video_filter_.sources) {
            sources_[p.first].push_back(p.second);
        }
        for (auto& p : audio_filter_.sources) {
            sources_[p.first].push_back(p.second);
        }

        std::vector<int> keys;
        // Flush unused inputs.
        for (auto& p : decoders_) {
            if (sources_.find(p.first) == sources_.end()) {
                keys.push_back(p.first);
            }
        }

        for (auto& key : keys) {
            decoders_.erase(key);
        }
    }

    std::string print() const
    {
        const int          position = std::max(static_cast<int>(time() - start().value_or(0)), 0);
        std::ostringstream str;
        str << std::fixed << std::setprecision(4) << "ffmpeg[" << name_ << "|"
            << av_q2d({position * format_tb_.num, format_tb_.den}) << "/"
            << av_q2d({static_cast<int>(duration().value_or(0LL)) * format_tb_.num, format_tb_.den}) << "]";
        return str.str();
    }
};

AVProducer::AVProducer(std::shared_ptr<core::frame_factory> frame_factory,
                       core::video_format_desc              format_desc,
                       std::string                          name,
                       std::string                          path,
                       std::optional<std::string>           vfilter,
                       std::optional<std::string>           afilter,
                       std::optional<int64_t>               start,
                       std::optional<int64_t>               seek,
                       std::optional<int64_t>               duration,
                       std::optional<bool>                  loop,
                       int                                  seekable,
                       core::frame_geometry::scale_mode     scale_mode)
    : impl_(new Impl(std::move(frame_factory),
                     std::move(format_desc),
                     std::move(name),
                     std::move(path),
                     std::move(vfilter.value_or("")),
                     std::move(afilter.value_or("")),
                     std::move(start),
                     std::move(seek),
                     std::move(duration),
                     std::move(loop.value_or(false)),
                     seekable,
                     scale_mode))
{
}

core::draw_frame AVProducer::next_frame(const core::video_field field) { return impl_->next_frame(field); }

core::draw_frame AVProducer::prev_frame(const core::video_field field) { return impl_->prev_frame(field); }

bool AVProducer::is_ready() { return impl_->is_ready(); }

AVProducer& AVProducer::seek(int64_t time)
{
    impl_->seek(time);
    return *this;
}

AVProducer& AVProducer::loop(bool loop)
{
    impl_->loop(loop);
    return *this;
}

bool AVProducer::loop() const { return impl_->loop(); }

AVProducer& AVProducer::start(int64_t start)
{
    impl_->start(start);
    return *this;
}

int64_t AVProducer::time() const { return impl_->time(); }

int64_t AVProducer::start() const { return impl_->start().value_or(0); }

AVProducer& AVProducer::duration(int64_t duration)
{
    impl_->duration(duration);
    return *this;
}

int64_t AVProducer::duration() const { return impl_->duration().value_or(std::numeric_limits<int64_t>::max()); }

core::monitor::state AVProducer::state() const
{
    boost::lock_guard<boost::mutex> lock(impl_->state_mutex_);
    return impl_->state_;
}

}} // namespace caspar::ffmpeg
