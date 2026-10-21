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
#include "omt_consumer.h"

#include "../util/omt_util.h"

#include <common/bit_depth.h>
#include <common/diagnostics/graph.h>
#include <common/except.h>
#include <common/future.h>
#include <common/log.h>
#include <common/os/thread.h>
#include <common/param.h>
#include <common/timer.h>
#include <common/utf.h>

#include <core/consumer/channel_info.h>
#include <core/consumer/frame_consumer.h>
#include <core/frame/frame.h>
#include <core/video_format.h>

#include <boost/algorithm/string.hpp>
#include <boost/property_tree/ptree.hpp>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace caspar { namespace omt {

namespace {

// Frames waiting for the send thread. The oldest one is dropped when the thread falls behind.
constexpr size_t max_queued_frames = 4;

// --- Settings ---

// An empty value means that the setting was not given. Any other value that is not understood
// falls back to the default, which is worth a warning.
void warn_unknown_value(const wchar_t* setting, const std::wstring& value, const wchar_t* fallback)
{
    CASPAR_LOG(warning) << L"OMT: unknown " << setting << L" \"" << value << L"\", using " << fallback << L".";
}

OMTQuality parse_quality(const std::wstring& value)
{
    if (boost::iequals(value, L"low") == true) {
        return OMTQuality_Low;
    }
    if (boost::iequals(value, L"medium") == true) {
        return OMTQuality_Medium;
    }
    if (boost::iequals(value, L"high") == true) {
        return OMTQuality_High;
    }
    if (value.empty() == false && boost::iequals(value, L"default") == false) {
        warn_unknown_value(L"QUALITY", value, L"default");
    }
    return OMTQuality_Default;
}

// CasparCG composes with premultiplied alpha. Some receivers handle alpha badly, so the frame
// can be converted to straight alpha, or sent without alpha.
enum class alpha_mode
{
    premultiplied,
    straight,
    none
};

alpha_mode parse_alpha_mode(const std::wstring& value)
{
    if (boost::iequals(value, L"straight") == true) {
        return alpha_mode::straight;
    }
    if (boost::iequals(value, L"none") == true) {
        return alpha_mode::none;
    }
    if (value.empty() == false && boost::iequals(value, L"premultiplied") == false) {
        warn_unknown_value(L"ALPHA", value, L"premultiplied");
    }
    return alpha_mode::premultiplied;
}

const wchar_t* alpha_mode_name(alpha_mode mode)
{
    switch (mode) {
        case alpha_mode::straight:
            return L"straight";
        case alpha_mode::none:
            return L"none";
        default:
            return L"premultiplied";
    }
}

// The uncompressed pixel format handed to libomt, which compresses it for the network.
// bgra is CasparCG's own buffer sent as it is. The others are converted with libswscale, for
// receivers that cannot decode BGRA with alpha.
enum class pixel_format
{
    bgra,
    uyvy,
    yuy2,
    uyva,
    nv12,
    yv12
};

pixel_format parse_pixel_format(const std::wstring& value)
{
    if (boost::iequals(value, L"uyvy") == true) {
        return pixel_format::uyvy;
    }
    if (boost::iequals(value, L"yuy2") == true) {
        return pixel_format::yuy2;
    }
    if (boost::iequals(value, L"uyva") == true) {
        return pixel_format::uyva;
    }
    if (boost::iequals(value, L"nv12") == true) {
        return pixel_format::nv12;
    }
    if (boost::iequals(value, L"yv12") == true) {
        return pixel_format::yv12;
    }
    if (value.empty() == false && boost::iequals(value, L"bgra") == false) {
        warn_unknown_value(L"PIXEL", value, L"bgra");
    }
    return pixel_format::bgra;
}

const wchar_t* pixel_format_name(pixel_format format)
{
    switch (format) {
        case pixel_format::uyvy:
            return L"UYVY";
        case pixel_format::yuy2:
            return L"YUY2";
        case pixel_format::uyva:
            return L"UYVA";
        case pixel_format::nv12:
            return L"NV12";
        case pixel_format::yv12:
            return L"YV12";
        default:
            return L"BGRA";
    }
}

bool pixel_format_supports_alpha(pixel_format format)
{
    return format == pixel_format::bgra || format == pixel_format::uyva;
}

// --- Alpha ---

// For each alpha value, 255 / alpha as a 16.16 fixed-point number. Multiplying by it replaces
// the division when converting premultiplied pixels to straight alpha.
struct unpremultiply_table
{
    std::array<uint32_t, 256> reciprocals;

    unpremultiply_table()
    {
        reciprocals[0] = 0; // Never used, fully transparent pixels are skipped.
        for (uint32_t alpha = 1; alpha < reciprocals.size(); ++alpha) {
            reciprocals[alpha] = (255u * 65536u + alpha / 2) / alpha;
        }
    }
};

// Converts BGRA pixels from premultiplied to straight alpha, in place.
void unpremultiply_bgra(uint8_t* data, size_t byte_count)
{
    static const unpremultiply_table table;

    for (size_t offset = 0; offset + 4 <= byte_count; offset += 4) {
        uint8_t* const pixel = data + offset;
        const uint8_t  alpha = pixel[3];

        // Both extremes look the same in premultiplied and straight alpha.
        if (alpha == 0 || alpha == 255) {
            continue;
        }

        const uint32_t reciprocal = table.reciprocals[alpha];
        for (int channel = 0; channel < 3; ++channel) {
            pixel[channel] = static_cast<uint8_t>(std::min<uint32_t>(255, (pixel[channel] * reciprocal + 32768) >> 16));
        }
    }
}

// --- FFmpeg ---

struct swr_deleter
{
    void operator()(SwrContext* context) { swr_free(&context); }
};

struct sws_deleter
{
    void operator()(SwsContext* context) { sws_freeContext(context); }
};

// The color space that the frames are tagged with. It is the one that receivers assume when the
// tag is missing: BT.601 below 720 lines and BT.709 from there on.
OMTColorSpace color_space_for_height(int height)
{
    if (height >= 720) {
        return OMTColorSpace_BT709;
    }
    return OMTColorSpace_BT601;
}

// libswscale converts RGB to YUV with the BT.601 matrix unless it is told otherwise, so the
// matrix is set to the one that the frames are tagged with. The BGRA source is full range, the
// YUV frames are limited range.
void set_conversion_matrix(SwsContext* context, OMTColorSpace color_space)
{
    const int        matrix       = color_space == OMTColorSpace_BT709 ? SWS_CS_ITU709 : SWS_CS_ITU601;
    const int* const coefficients = sws_getCoefficients(matrix);
    constexpr int    source_range = 1;
    constexpr int    target_range = 0;
    constexpr int    unity        = 1 << 16;

    const int result =
        sws_setColorspaceDetails(context, coefficients, source_range, coefficients, target_range, 0, unity, unity);
    if (result < 0) {
        CASPAR_LOG(warning) << L"OMT: libswscale did not accept the color matrix, the colors may be off.";
    }
}

} // namespace

struct omt_consumer : public core::frame_consumer
{
    static std::atomic<int> instances_;
    const int               instance_no_;
    const std::wstring      name_;
    const std::string       name_utf8_;
    const bool              name_sanitized_;
    const OMTQuality        quality_;
    const alpha_mode        alpha_mode_;
    const bool              alpha_requested_;
    const pixel_format      pixel_format_;

    core::video_format_desc format_desc_;
    int                     channel_index_ = 0;

    omt_lib*    lib_  = nullptr;
    omt_send_t* send_ = nullptr;

    std::unique_ptr<SwrContext, swr_deleter> swr_;
    std::unique_ptr<SwsContext, sws_deleter> sws_;
    AVPixelFormat                            sws_format_ = AV_PIX_FMT_NONE;
    int                                      sws_width_  = 0;
    int                                      sws_height_ = 0;

    // Buffers that are reused for every frame.
    std::vector<uint8_t>  straight_alpha_buffer_;
    std::vector<uint8_t>  pixel_buffer_;
    std::vector<float>    audio_buffer_;
    std::vector<uint8_t*> audio_planes_;

    std::mutex                    frame_queue_mutex_;
    std::condition_variable       frame_queue_cond_;
    std::queue<core::const_frame> frame_queue_;
    std::atomic<bool>             running_{false};
    std::thread                   send_thread_;

    spl::shared_ptr<diagnostics::graph> graph_;
    caspar::timer                       tick_timer_;
    caspar::timer                       frame_timer_;

  public:
    omt_consumer(std::wstring name, OMTQuality quality, alpha_mode alpha, bool alpha_requested, pixel_format format)
        : instance_no_(instances_++)
        , name_(name.empty() == false ? name : default_name())
        , name_utf8_(u8(omt::make_ascii_safe_name(name_)))
        , name_sanitized_(u16(name_utf8_) != name_)
        , quality_(quality)
        , alpha_mode_(alpha)
        , alpha_requested_(alpha_requested)
        , pixel_format_(format)
    {
        lib_ = omt::load_library();

        graph_ = spl::make_shared<diagnostics::graph>();
        graph_->set_text(print());
        graph_->set_color("frame-time", diagnostics::color(0.5f, 1.0f, 0.2f));
        graph_->set_color("tick-time", diagnostics::color(0.0f, 0.6f, 0.9f));
        graph_->set_color("buffered-frames", diagnostics::color(0.5f, 0.0f, 0.2f));
        diagnostics::register_graph(graph_);
    }

    ~omt_consumer() { stop_sending(); }

    // --- core::frame_consumer ---

    void initialize(const core::video_format_desc& format_desc,
                    const core::channel_info&      channel_info,
                    int                            port_index) override
    {
        // The output initializes a consumer again when the video format of the channel changes.
        stop_sending();

        format_desc_   = format_desc;
        channel_index_ = channel_info.index;
        require_even_size();

        if (name_sanitized_ == true) {
            CASPAR_LOG(warning) << print() << L" OMT sender name \"" << name_ << L"\" contains non-ASCII characters, "
                                << L"using \"" << u16(name_utf8_) << L"\" instead. libomt breaks discovery of a "
                                << L"sender with a non-ASCII name when there is more than one sender.";
        }

        {
            auto lock = omt::serialize_create_call();
            send_     = lib_->send_create(name_utf8_.c_str(), quality_);
        }
        if (send_ == nullptr) {
            CASPAR_THROW_EXCEPTION(caspar_exception() << msg_info(u8(print()) + " Failed to create the OMT sender."));
        }
        log_discovery_address();

        create_audio_converter();
        graph_->set_text(print());
        log_output_format();

        running_     = true;
        send_thread_ = std::thread([this] { run(); });
    }

    std::future<bool> send(const core::video_field /*field*/, core::const_frame frame) override
    {
        graph_->set_value("tick-time", tick_timer_.elapsed() * format_desc_.fps * 0.5);
        tick_timer_.restart();

        {
            std::lock_guard<std::mutex> lock(frame_queue_mutex_);
            frame_queue_.push(std::move(frame));
            if (frame_queue_.size() > max_queued_frames) {
                frame_queue_.pop();
            }
        }
        frame_queue_cond_.notify_all();

        return caspar::make_ready_future(true);
    }

    std::wstring print() const override
    {
        if (channel_index_ != 0) {
            return L"omt[" + std::to_wstring(channel_index_) + L"|" + name_ + L"]";
        }
        return L"[omt_consumer]";
    }

    std::wstring name() const override { return L"omt"; }

    int index() const override { return 910; }

    bool has_synchronization_clock() const override { return false; }

    core::monitor::state state() const override
    {
        core::monitor::state state;
        state["omt/name"]  = u8(name_);
        state["omt/pixel"] = u8(pixel_format_name(pixel_format_));
        state["omt/alpha"] = u8(alpha_mode_name(alpha_mode_));
        return state;
    }

  private:
    // Stops the send thread, drops the queued frames and destroys the sender.
    void stop_sending()
    {
        running_ = false;
        frame_queue_cond_.notify_all();
        if (send_thread_.joinable() == true) {
            send_thread_.join();
        }

        {
            std::lock_guard<std::mutex> lock(frame_queue_mutex_);
            frame_queue_ = std::queue<core::const_frame>();
        }

        if (send_ != nullptr) {
            auto lock = omt::serialize_create_call();
            lib_->send_destroy(send_);
            send_ = nullptr;
        }
    }

    // The chroma planes of the subsampled formats are laid out for even sizes, and the buffers are sized that way.
    void require_even_size() const
    {
        const bool subsampled = pixel_format_ != pixel_format::bgra;
        const bool even       = format_desc_.width % 2 == 0 && format_desc_.height % 2 == 0;
        if (subsampled == true && even == false) {
            const std::string size = std::to_string(format_desc_.width) + "x" + std::to_string(format_desc_.height);
            CASPAR_THROW_EXCEPTION(caspar_exception()
                                   << msg_info(u8(print()) + " " + u8(pixel_format_name(pixel_format_)) +
                                               " needs an even frame size, the channel is " + size + "."));
        }
    }

    // The first consumer is called "CasparCG", the next ones "CasparCG 1", "CasparCG 2" and so on.
    std::wstring default_name() const
    {
        if (instance_no_ == 0) {
            return L"CasparCG";
        }
        return L"CasparCG " + std::to_wstring(instance_no_);
    }

    // Logs the name that receivers see this sender under.
    void log_discovery_address()
    {
        if (send_ == nullptr) {
            return;
        }

        char address[512] = {};
        lib_->send_getaddress(send_, address, static_cast<int>(sizeof(address)));
        CASPAR_LOG(info) << print() << L" OMT discovery address: \"" << u16(std::string(address)) << L"\"";
    }

    void log_output_format() const
    {
        if (pixel_format_supports_alpha(pixel_format_) == true) {
            CASPAR_LOG(info) << print() << L" sending " << pixel_format_name(pixel_format_) << L" with "
                             << alpha_mode_name(alpha_mode_) << L" alpha.";
            return;
        }

        CASPAR_LOG(info) << print() << L" sending " << pixel_format_name(pixel_format_) << L" (no alpha channel).";
        if (alpha_requested_ == true && alpha_mode_ != alpha_mode::none) {
            CASPAR_LOG(warning) << print() << L" " << pixel_format_name(pixel_format_)
                                << L" cannot carry an alpha channel, ignoring ALPHA " << alpha_mode_name(alpha_mode_)
                                << L".";
        }
    }

    // CasparCG audio is interleaved 32 bit integers, libomt wants planar floats.
    void create_audio_converter()
    {
        AVChannelLayout layout;
        av_channel_layout_default(&layout, format_desc_.audio_channels);

        SwrContext* context = nullptr;
        swr_alloc_set_opts2(&context,
                            &layout,
                            AV_SAMPLE_FMT_FLTP,
                            format_desc_.audio_sample_rate,
                            &layout,
                            AV_SAMPLE_FMT_S32,
                            format_desc_.audio_sample_rate,
                            0,
                            nullptr);
        swr_.reset(context);
        swr_init(context);
    }

    // --- Send thread ---

    void run()
    {
        set_thread_name(L"OMT-SEND: " + name_);

        while (running_ == true) {
            core::const_frame frame;
            if (wait_for_frame(frame) == false) {
                continue;
            }

            // An exception that leaves this thread ends the server, so a frame that cannot be sent is lost instead.
            try {
                frame_timer_.restart();
                send_video(frame);
                send_audio(frame);
                graph_->set_value("frame-time", frame_timer_.elapsed() * format_desc_.fps * 0.5);
            } catch (...) {
                CASPAR_LOG_CURRENT_EXCEPTION();
            }
        }
    }

    // Takes the oldest queued frame. Returns false if none arrived within 100 ms, or on shutdown.
    bool wait_for_frame(core::const_frame& frame)
    {
        std::unique_lock<std::mutex> lock(frame_queue_mutex_);

        frame_queue_cond_.wait_for(
            lock, std::chrono::milliseconds(100), [&] { return frame_queue_.empty() == false || running_ == false; });
        if (running_ == false || frame_queue_.empty() == true) {
            return false;
        }

        frame = std::move(frame_queue_.front());
        frame_queue_.pop();

        const double queued_frames = static_cast<double>(frame_queue_.size()) + 0.001;
        graph_->set_value("buffered-frames", queued_frames / 8);
        return true;
    }

    // --- Video ---

    void send_video(const core::const_frame& frame)
    {
        const auto image = frame.image_data(0);
        if (send_ == nullptr || image.size() == 0) {
            return;
        }

        const int width  = static_cast<int>(format_desc_.width);
        const int height = static_cast<int>(format_desc_.height);

        const uint8_t* bgra  = image.data();
        OMTVideoFlags  flags = OMTVideoFlags_None;
        if (pixel_format_supports_alpha(pixel_format_) == true) {
            flags = prepare_alpha(bgra, image.size());
        }

        OMTMediaFrame video_frame = {};
        video_frame.Type          = OMTFrameType_Video;
        video_frame.Timestamp     = -1; // libomt creates the timestamps and paces the frames by FrameRateN/D.
        video_frame.Width         = width;
        video_frame.Height        = height;
        video_frame.Flags         = flags;
        // An interlaced channel hands over every field as a frame, so twice as many frames as its frame rate.
        video_frame.FrameRateN = format_desc_.framerate.numerator() * format_desc_.field_count;
        video_frame.FrameRateD = format_desc_.framerate.denominator();
        video_frame.AspectRatio =
            static_cast<float>(format_desc_.square_width) / static_cast<float>(format_desc_.square_height);
        video_frame.ColorSpace = color_space_for_height(height);

        switch (pixel_format_) {
            case pixel_format::uyvy:
                fill_packed_422(video_frame, AV_PIX_FMT_UYVY422, OMTCodec_UYVY, bgra);
                break;
            case pixel_format::yuy2:
                fill_packed_422(video_frame, AV_PIX_FMT_YUYV422, OMTCodec_YUY2, bgra);
                break;
            case pixel_format::uyva:
                fill_uyva(video_frame, bgra);
                break;
            case pixel_format::nv12:
                fill_nv12(video_frame, bgra);
                break;
            case pixel_format::yv12:
                fill_yv12(video_frame, bgra);
                break;
            case pixel_format::bgra:
            default:
                video_frame.Codec      = OMTCodec_BGRA;
                video_frame.Stride     = width * 4;
                video_frame.Data       = const_cast<uint8_t*>(bgra);
                video_frame.DataLength = static_cast<int>(image.size());
                break;
        }

        // libomt encodes or copies the data before it returns, so the buffers can be reused for the next frame.
        lib_->send(send_, &video_frame);
    }

    // Returns the alpha flags for the frame. Straight alpha needs converted pixels, `bgra` is
    // pointed at a converted copy then.
    OMTVideoFlags prepare_alpha(const uint8_t*& bgra, size_t byte_count)
    {
        switch (alpha_mode_) {
            case alpha_mode::none:
                // Without the Alpha flag receivers ignore the alpha data (BGRX/UYVY instead of BGRA/UYVA).
                return OMTVideoFlags_None;

            case alpha_mode::straight:
                // CasparCG's frames are premultiplied, so convert a copy for receivers that
                // do not understand OMTVideoFlags_PreMultiplied.
                straight_alpha_buffer_.assign(bgra, bgra + byte_count);
                unpremultiply_bgra(straight_alpha_buffer_.data(), straight_alpha_buffer_.size());
                bgra = straight_alpha_buffer_.data();
                return OMTVideoFlags_Alpha;

            case alpha_mode::premultiplied:
            default:
                return static_cast<OMTVideoFlags>(OMTVideoFlags_Alpha | OMTVideoFlags_PreMultiplied);
        }
    }

    // Converts the BGRA frame into pixel_buffer_ with libswscale.
    // The destination planes and strides are described the way libswscale expects them.
    void convert_bgra(AVPixelFormat                  format,
                      const uint8_t*                 bgra,
                      int                            width,
                      int                            height,
                      const std::array<uint8_t*, 4>& planes,
                      const std::array<int, 4>&      strides)
    {
        const bool context_matches =
            sws_ != nullptr && sws_format_ == format && sws_width_ == width && sws_height_ == height;
        if (context_matches == false) {
            sws_format_ = format;
            sws_width_  = width;
            sws_height_ = height;
            sws_.reset(sws_getContext(
                width, height, AV_PIX_FMT_BGRA, width, height, format, SWS_POINT, nullptr, nullptr, nullptr));
            if (sws_ != nullptr) {
                set_conversion_matrix(sws_.get(), color_space_for_height(height));
            }
        }

        const std::array<const uint8_t*, 4> source_planes  = {bgra, nullptr, nullptr, nullptr};
        const std::array<int, 4>            source_strides = {width * 4, 0, 0, 0};
        sws_scale(sws_.get(), source_planes.data(), source_strides.data(), 0, height, planes.data(), strides.data());
    }

    // UYVY and YUY2: one plane, 2 bytes per pixel.
    void fill_packed_422(OMTMediaFrame& video_frame, AVPixelFormat format, OMTCodec codec, const uint8_t* bgra)
    {
        const int width  = video_frame.Width;
        const int height = video_frame.Height;

        pixel_buffer_.resize(static_cast<size_t>(width) * height * 2);
        convert_bgra(
            format, bgra, width, height, {pixel_buffer_.data(), nullptr, nullptr, nullptr}, {width * 2, 0, 0, 0});

        video_frame.Codec      = codec;
        video_frame.Stride     = width * 2;
        video_frame.Data       = pixel_buffer_.data();
        video_frame.DataLength = static_cast<int>(pixel_buffer_.size());
    }

    // UYVY followed by a plane with one alpha byte per pixel.
    void fill_uyva(OMTMediaFrame& video_frame, const uint8_t* bgra)
    {
        const int    width       = video_frame.Width;
        const int    height      = video_frame.Height;
        const size_t pixel_count = static_cast<size_t>(width) * height;
        const size_t uyvy_size   = pixel_count * 2;
        const bool   has_alpha   = (video_frame.Flags & OMTVideoFlags_Alpha) != 0;

        pixel_buffer_.resize(uyvy_size + (has_alpha == true ? pixel_count : 0));
        convert_bgra(AV_PIX_FMT_UYVY422,
                     bgra,
                     width,
                     height,
                     {pixel_buffer_.data(), nullptr, nullptr, nullptr},
                     {width * 2, 0, 0, 0});

        if (has_alpha == true) {
            uint8_t* const alpha_plane = pixel_buffer_.data() + uyvy_size;
            for (size_t pixel = 0; pixel < pixel_count; ++pixel) {
                alpha_plane[pixel] = bgra[pixel * 4 + 3];
            }
            video_frame.Codec = OMTCodec_UYVA;
        } else {
            video_frame.Codec = OMTCodec_UYVY; // No alpha plane was added, so it is plain UYVY.
        }

        video_frame.Stride     = width * 2;
        video_frame.Data       = pixel_buffer_.data();
        video_frame.DataLength = static_cast<int>(pixel_buffer_.size());
    }

    // A luma plane followed by one plane with interleaved U and V at half the height.
    void fill_nv12(OMTMediaFrame& video_frame, const uint8_t* bgra)
    {
        const int    width     = video_frame.Width;
        const int    height    = video_frame.Height;
        const size_t luma_size = static_cast<size_t>(width) * height;

        pixel_buffer_.resize(luma_size + luma_size / 2);
        uint8_t* const luma_plane = pixel_buffer_.data();
        uint8_t* const uv_plane   = luma_plane + luma_size;
        convert_bgra(
            AV_PIX_FMT_NV12, bgra, width, height, {luma_plane, uv_plane, nullptr, nullptr}, {width, width, 0, 0});

        video_frame.Codec      = OMTCodec_NV12;
        video_frame.Stride     = width;
        video_frame.Data       = pixel_buffer_.data();
        video_frame.DataLength = static_cast<int>(pixel_buffer_.size());
    }

    // Three planes: luma, then V, then U, both chroma planes at half the width and height.
    void fill_yv12(OMTMediaFrame& video_frame, const uint8_t* bgra)
    {
        const int    width       = video_frame.Width;
        const int    height      = video_frame.Height;
        const size_t luma_size   = static_cast<size_t>(width) * height;
        const size_t chroma_size = luma_size / 4;

        pixel_buffer_.resize(luma_size + chroma_size * 2);
        uint8_t* const luma_plane = pixel_buffer_.data();
        uint8_t* const v_plane    = luma_plane + luma_size;
        uint8_t* const u_plane    = v_plane + chroma_size;

        // libswscale's planar 4:2:0 format writes Y, U, V into the pointers in that order. YV12
        // keeps V first in memory, so the U pointer is the one that points at the second chroma plane.
        convert_bgra(AV_PIX_FMT_YUV420P,
                     bgra,
                     width,
                     height,
                     {luma_plane, u_plane, v_plane, nullptr},
                     {width, width / 2, width / 2, 0});

        video_frame.Codec      = OMTCodec_YV12;
        video_frame.Stride     = width;
        video_frame.Data       = pixel_buffer_.data();
        video_frame.DataLength = static_cast<int>(pixel_buffer_.size());
    }

    // --- Audio ---

    void send_audio(const core::const_frame& frame)
    {
        if (send_ == nullptr || swr_ == nullptr) {
            return;
        }

        const auto audio_in            = frame.audio_data();
        const int  channels            = format_desc_.audio_channels;
        const int  samples_per_channel = channels > 0 ? static_cast<int>(audio_in.size()) / channels : 0;
        if (samples_per_channel <= 0) {
            return;
        }

        // Planar: all samples of the first channel, then all samples of the second channel and so on.
        audio_buffer_.assign(static_cast<size_t>(channels) * samples_per_channel, 0.0f);
        audio_planes_.resize(channels);
        for (int channel = 0; channel < channels; ++channel) {
            float* const plane     = audio_buffer_.data() + static_cast<size_t>(channel) * samples_per_channel;
            audio_planes_[channel] = reinterpret_cast<uint8_t*>(plane);
        }

        const uint8_t* input = reinterpret_cast<const uint8_t*>(audio_in.data());
        swr_convert(swr_.get(), audio_planes_.data(), samples_per_channel, &input, samples_per_channel);

        OMTMediaFrame audio_frame     = {};
        audio_frame.Type              = OMTFrameType_Audio;
        audio_frame.Timestamp         = -1;
        audio_frame.Codec             = OMTCodec_FPA1;
        audio_frame.SampleRate        = format_desc_.audio_sample_rate;
        audio_frame.Channels          = channels;
        audio_frame.SamplesPerChannel = samples_per_channel;
        audio_frame.Data              = audio_buffer_.data();
        audio_frame.DataLength        = static_cast<int>(audio_buffer_.size() * sizeof(float));

        // As above, libomt is done with audio_buffer_ when it returns.
        lib_->send(send_, &audio_frame);
    }
};

std::atomic<int> omt_consumer::instances_(0);

namespace {

// The frames are handed to libomt as 8-bit BGRA, a deeper channel would be sent as garbage.
void require_8_bit_channel(const core::channel_info& channel_info)
{
    if (channel_info.depth != common::bit_depth::bit8) {
        CASPAR_THROW_EXCEPTION(caspar_exception() << msg_info("The OMT consumer only supports 8-bit color depth."));
    }
}

spl::shared_ptr<core::frame_consumer> make_consumer(const std::wstring& name,
                                                    const std::wstring& quality,
                                                    const std::wstring& alpha,
                                                    const std::wstring& pixel)
{
    return spl::make_shared<omt_consumer>(
        name, parse_quality(quality), parse_alpha_mode(alpha), alpha.empty() == false, parse_pixel_format(pixel));
}

} // namespace

spl::shared_ptr<core::frame_consumer>
create_omt_consumer(const std::vector<std::wstring>&                         params,
                    const core::video_format_repository&                     format_repository,
                    const std::vector<spl::shared_ptr<core::video_channel>>& channels,
                    const core::channel_info&                                channel_info)
{
    if (params.empty() == true || boost::iequals(params.at(0), L"OMT") == false) {
        return core::frame_consumer::empty();
    }

    require_8_bit_channel(channel_info);

    return make_consumer(get_param(L"NAME", params, L""),
                         get_param(L"QUALITY", params, L""),
                         get_param(L"ALPHA", params, L""),
                         get_param(L"PIXEL", params, L""));
}

spl::shared_ptr<core::frame_consumer>
create_preconfigured_omt_consumer(const boost::property_tree::wptree&                      element,
                                  const core::video_format_repository&                     format_repository,
                                  const std::vector<spl::shared_ptr<core::video_channel>>& channels,
                                  const core::channel_info&                                channel_info)
{
    require_8_bit_channel(channel_info);

    return make_consumer(element.get(L"name", L""),
                         element.get(L"quality", L""),
                         element.get(L"alpha", L""),
                         element.get(L"pixel", L""));
}

}} // namespace caspar::omt
