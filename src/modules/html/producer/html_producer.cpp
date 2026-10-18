/*
 * Copyright 2013 Sveriges Television AB http://casparcg.com/
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

#include "html_producer.h"

#include <core/video_format.h>

#include <core/frame/draw_frame.h>
#include <core/frame/frame.h>
#include <core/frame/frame_factory.h>
#include <core/frame/frame_transform.h>
#include <core/frame/geometry.h>
#include <core/frame/pixel_format.h>
#include <core/monitor/monitor.h>
#include <core/producer/frame_producer.h>

#include <common/assert.h>
#include <common/diagnostics/graph.h>
#include <common/env.h>
#include <common/except.h>
#include <common/future.h>
#include <common/os/filesystem.h>
#include <common/timer.h>

#if defined(__GNUC__) && __GNUC__ == 14
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#endif
#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/regex.hpp>
#if defined(__GNUC__) && __GNUC__ == 14
#pragma GCC diagnostic pop
#endif

#include <tbb/concurrent_queue.h>
#include <tbb/parallel_for.h>

#include <chrono>
#include <deque>
#include <mutex>

#include <include/cef_app.h>
#include <include/cef_client.h>
#include <include/cef_render_handler.h>

#include <optional>
#include <queue>
#include <utility>

#include <ffmpeg/util/audio_resampler.h>

#include "../html.h"
#include "../util.h"

#ifdef WIN32
#include <accelerator/d3d/d3d_device.h>
#include <accelerator/d3d/d3d_device_context.h>
#include <accelerator/d3d/d3d_texture2d.h>
#endif

namespace caspar { namespace html {

inline std::int_least64_t now()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::high_resolution_clock::now().time_since_epoch())
        .count();
}

struct presentation_frame
{
    std::int_least64_t timestamp;
    core::draw_frame   frame;

    explicit presentation_frame(core::draw_frame frame = {}, std::int_least64_t ts = now()) noexcept
        : timestamp(ts)
        , frame(std::move(frame))
    {
    }

    presentation_frame(presentation_frame&& other) noexcept
        : timestamp(other.timestamp)
        , frame(std::move(other.frame))
    {
    }

    presentation_frame(const presentation_frame&)            = delete;
    presentation_frame& operator=(const presentation_frame&) = delete;

    presentation_frame& operator=(presentation_frame&& rhs) noexcept
    {
        timestamp = rhs.timestamp;
        frame     = std::move(rhs.frame);
        return *this;
    }

    ~presentation_frame() {}
};

class html_client
    : public CefClient
    , public CefRenderHandler
    , public CefAudioHandler
    , public CefLifeSpanHandler
    , public CefLoadHandler
    , public CefDisplayHandler
    , public CefRequestHandler
{
    std::wstring                        url_;
    spl::shared_ptr<diagnostics::graph> graph_;
    core::monitor::state                state_;
    mutable std::mutex                  state_mutex_;
    caspar::timer                       tick_timer_;
    caspar::timer                       frame_timer_;
    caspar::timer                       paint_timer_;
    caspar::timer                       test_timer_;

    spl::shared_ptr<core::frame_factory> frame_factory_;
    core::video_format_desc              format_desc_;
    bool                                 gpu_enabled_;
    bool                                 shared_texture_enable_;
    tbb::concurrent_queue<std::wstring>  javascript_before_load_;
    std::mutex                          javascript_mutex_;
    bool                                recovery_stopped_ = false;
    std::atomic<bool>                    loaded_;
    std::atomic<bool>                    not_found_;
    std::queue<presentation_frame>       frames_;
    std::queue<presentation_frame>       audio_frames_;
    mutable std::mutex                   frames_mutex_;
    mutable std::mutex                   audio_frames_mutex_;
    const size_t                         frames_max_size_ = 4;
    std::atomic<bool>                    closing_;

    std::deque<std::chrono::steady_clock::time_point> renderer_termination_times_;

    std::unique_ptr<ffmpeg::AudioResampler> audioResampler_;

    core::draw_frame   last_video_frame_;
    core::draw_frame   last_frame_;
    std::int_least64_t last_frame_time_;

    CefRefPtr<CefBrowser> browser_;

#ifdef WIN32
    std::shared_ptr<accelerator::d3d::d3d_device> const d3d_device_;
    std::shared_ptr<accelerator::d3d::d3d_texture2d>    d3d_shared_buffer_;
#endif

  public:
    html_client(spl::shared_ptr<core::frame_factory>       frame_factory,
                const spl::shared_ptr<diagnostics::graph>& graph,
                core::video_format_desc                    format_desc,
                bool                                       gpu_enabled,
                bool                                       shared_texture_enable,
                std::wstring                               url)
        : url_(std::move(url))
        , graph_(graph)
        , frame_factory_(std::move(frame_factory))
        , format_desc_(std::move(format_desc))
        , gpu_enabled_(gpu_enabled)
        , shared_texture_enable_(shared_texture_enable)
#ifdef WIN32
        , d3d_device_(accelerator::d3d::d3d_device::get_device())
#endif
    {
        graph_->set_color("browser-tick-time", diagnostics::color(0.1f, 1.0f, 0.1f));
        graph_->set_color("tick-time", diagnostics::color(0.0f, 0.6f, 0.9f));
        graph_->set_color("dropped-frame", diagnostics::color(0.3f, 0.6f, 0.3f));
        graph_->set_color("late-frame", diagnostics::color(0.6f, 0.1f, 0.1f));
        graph_->set_color("overload", diagnostics::color(0.6f, 0.6f, 0.3f));
        graph_->set_color("buffered-frames", diagnostics::color(0.2f, 0.9f, 0.9f));
        graph_->set_text(print());
        diagnostics::register_graph(graph_);

        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            state_["file/path"] = u8(url_);
        }

        loaded_    = false;
        not_found_ = false;
        closing_   = false;
    }

    void reload()
    {
        html::invoke([this] {
            if (browser_ != nullptr) {
                {
                    std::lock_guard<std::mutex> lock(javascript_mutex_);
                    renderer_termination_times_.clear();
                    loaded_           = false;
                    recovery_stopped_ = false;
                }
                browser_->Reload();
            }
        });
    }

    void close()
    {
        closing_ = true;

        html::invoke([this] {
            if (browser_ != nullptr) {
                browser_->GetHost()->CloseBrowser(true);
            }
        });
    }

    bool try_pop(const core::video_field field)
    {
        bool                        result = false;
        std::lock_guard<std::mutex> lock(frames_mutex_);

        core::draw_frame audio_frame;
        uint64_t         audio_frame_timestamp = 0;

        {
            std::lock_guard<std::mutex> audio_lock(audio_frames_mutex_);
            if (!audio_frames_.empty()) {
                audio_frame_timestamp = audio_frames_.front().timestamp;
                audio_frame           = core::draw_frame(std::move(audio_frames_.front().frame));
                audio_frames_.pop();
            }
        }

        if (!frames_.empty()) {
            /*
             * CEF in gpu-enabled mode only sends frames when something changes, and interlaced channels
             * consume two frames in a short time span.
             * This can interact poorly and cause the second
             * field of an animation repeat the first.
             * If there is a single field in the buffer, it may
             * want delaying to avoid this stutter.
             * The hazard here is that sometimes animations will
             * start a field later than intended.
             */
            if (field == core::video_field::a && frames_.size() == 1) {
                auto now_time = now();

                // Make sure there has been a gap before this pop, of at least a couple of frames
                auto follows_gap_in_frames = (now_time - last_frame_time_) > 100;

                // Check if the sole buffered frame is too young to have a partner field generated (with a tolerance)
                auto time_per_frame           = (1000 * 1.5) / format_desc_.fps;
                auto front_frame_is_too_young = (now_time - frames_.front().timestamp) < time_per_frame;

                if (follows_gap_in_frames && front_frame_is_too_young) {
                    return false;
                }
            }

            last_frame_time_  = frames_.front().timestamp;
            last_video_frame_ = std::move(frames_.front().frame);
            last_frame_       = last_video_frame_;
            frames_.pop();

            graph_->set_value("buffered-frames", (double)frames_.size() / frames_max_size_);

            result = true;
        }

        if (audio_frame) {
            last_frame_time_ = audio_frame_timestamp;
            last_frame_      = core::draw_frame::over(last_video_frame_, audio_frame);
            result           = true;
        }

        return result;
    }

    core::draw_frame receive(const core::video_field field)
    {
        if (!try_pop(field)) {
            graph_->set_tag(diagnostics::tag_severity::SILENT, "late-frame");
            return core::draw_frame::still(last_frame_);
        } else {
            return last_frame_;
        }
    }

    core::draw_frame last_frame() const { return core::draw_frame::still(last_frame_); }

    bool is_ready() const
    {
        std::lock_guard<std::mutex> lock(frames_mutex_);
        return !frames_.empty() || last_frame_;
    }

    void execute_javascript(const std::wstring& javascript)
    {
        std::lock_guard<std::mutex> lock(javascript_mutex_);
        if (recovery_stopped_)
            CASPAR_THROW_EXCEPTION(invalid_operation()
                                   << msg_info(print() + L" renderer recovery stopped; "
                                                        L"manual RELOAD or REMOVE/LOAD is required."));

        if (!loaded_) {
            javascript_before_load_.push(javascript);
        } else {
            execute_queued_javascript();
            do_execute_javascript(javascript);
        }
    }

    bool OnBeforePopup(CefRefPtr<CefBrowser>                      browser,
                       CefRefPtr<CefFrame>                        frame,
                       int                                        popup_id,
                       const CefString&                           target_url,
                       const CefString&                           target_frame_name,
                       CefLifeSpanHandler::WindowOpenDisposition  target_disposition,
                       bool                                       user_gesture,
                       const CefPopupFeatures&                    popupFeatures,
                       CefWindowInfo&                             windowInfo,
                       CefRefPtr<CefClient>&                      client,
                       CefBrowserSettings&                        settings,
                       CefRefPtr<CefDictionaryValue>&             dict,
                       bool*                                      no_javascript_access) override
    {
        // This blocks popup windows from opening, as they dont make sense and hit an exception in get_browser_host upon
        // closing
        return true;
    }

    CefRefPtr<CefBrowserHost> get_browser_host() const
    {
        if (browser_ != nullptr)
            return browser_->GetHost();
        return nullptr;
    }

    core::monitor::state state() const
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return state_;
    }

  private:
    void GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect) override
    {
        CASPAR_ASSERT(CefCurrentlyOn(TID_UI));

        rect = CefRect(0, 0, format_desc_.square_width, format_desc_.square_height);
    }

    void OnPaint(CefRefPtr<CefBrowser> browser,
                 PaintElementType      type,
                 const RectList&       dirtyRects,
                 const void*           buffer,
                 int                   width,
                 int                   height) override
    {
        if (shared_texture_enable_ || closing_ || not_found_)
            return;

        graph_->set_value("browser-tick-time", paint_timer_.elapsed() * format_desc_.fps * 0.5);
        paint_timer_.restart();
        CASPAR_ASSERT(CefCurrentlyOn(TID_UI));

        if (type != PET_VIEW)
            return;

        core::pixel_format_desc pixel_desc(core::pixel_format::bgra);
        pixel_desc.planes.emplace_back(width, height, 4);

        core::mutable_frame frame = frame_factory_->create_frame(this, pixel_desc);
        char*               src   = (char*)buffer;
        char*               dst   = reinterpret_cast<char*>(frame.image_data(0).begin());
        test_timer_.restart();

#ifdef WIN32
        if (gpu_enabled_) {
            int chunksize = height * width;
            tbb::parallel_for(0, 4, [&](int y) { std::memcpy(dst + y * chunksize, src + y * chunksize, chunksize); });
        } else {
            std::memcpy(dst, src, width * height * 4);
        }
#else
        // On my one test linux machine, doing a single memcpy doesn't have the same cost as windows,
        // making using tbb excessive
        std::memcpy(dst, src, width * height * 4);
#endif

        graph_->set_value("memcpy", test_timer_.elapsed() * format_desc_.fps * 0.5 * 5);

        {
            std::lock_guard<std::mutex> lock(frames_mutex_);

            core::draw_frame new_frame = core::draw_frame(std::move(frame));

            frames_.push(presentation_frame(std::move(new_frame)));
            while (frames_.size() > frames_max_size_) {
                frames_.pop();
                graph_->set_tag(diagnostics::tag_severity::WARNING, "dropped-frame");
            }
            graph_->set_value("buffered-frames", (double)frames_.size() / frames_max_size_);
        }
    }

#ifdef WIN32
    void OnAcceleratedPaint(CefRefPtr<CefBrowser>          browser,
                            PaintElementType               type,
                            const RectList&                dirtyRects,
                            const CefAcceleratedPaintInfo& info) override
    {
        try {
            if (!shared_texture_enable_ || closing_ || not_found_)
                return;

            graph_->set_value("browser-tick-time", paint_timer_.elapsed() * format_desc_.fps * 0.5);
            paint_timer_.restart();
            CASPAR_ASSERT(CefCurrentlyOn(TID_UI));

            if (type != PET_VIEW)
                return;

            if (d3d_shared_buffer_) {
                if (info.shared_texture_handle != d3d_shared_buffer_->share_handle())
                    d3d_shared_buffer_.reset();
            }

            if (!d3d_shared_buffer_) {
                d3d_shared_buffer_ = d3d_device_->open_shared_texture(info.shared_texture_handle);
                if (!d3d_shared_buffer_)
                    CASPAR_LOG(error) << print() << L" could not open shared texture!";
            }

            if (d3d_shared_buffer_) {
                core::pixel_format format = core::pixel_format::invalid;
                if (d3d_shared_buffer_->format() == DXGI_FORMAT_B8G8R8A8_UNORM) {
                    format = core::pixel_format::bgra;
                } else if (d3d_shared_buffer_->format() == DXGI_FORMAT_R8G8B8A8_UNORM) {
                    format = core::pixel_format::rgba;
                }

                if (format != core::pixel_format::invalid) {
                    auto frame =
                        frame_factory_->import_d3d_texture(this, d3d_shared_buffer_, format, common::bit_depth::bit8);
                    core::draw_frame dframe(std::move(frame));

                    {
                        std::lock_guard<std::mutex> lock(frames_mutex_);

                        frames_.push(presentation_frame(std::move(dframe)));
                        while (frames_.size() > 4) {
                            frames_.pop();
                            graph_->set_tag(diagnostics::tag_severity::WARNING, "dropped-frame");
                        }
                        graph_->set_value("buffered-frames", (double)frames_.size() / frames_max_size_);
                    }
                }
            }
        } catch (...) {
            CASPAR_LOG_CURRENT_EXCEPTION();
        }
    }
#endif

    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override
    {
        CASPAR_ASSERT(CefCurrentlyOn(TID_UI));

        browser_ = std::move(browser);
    }

    void OnBeforeClose(CefRefPtr<CefBrowser> browser) override
    {
        CASPAR_ASSERT(CefCurrentlyOn(TID_UI));

        browser_ = nullptr;
    }

    bool DoClose(CefRefPtr<CefBrowser> browser) override
    {
        CASPAR_ASSERT(CefCurrentlyOn(TID_UI));

        return false;
    }

    bool OnConsoleMessage(CefRefPtr<CefBrowser> browser,
                          cef_log_severity_t    level,
                          const CefString&      message,
                          const CefString&      source,
                          int                   line) override
    {
        auto msg = log::replace_nonprintable_copy(message.ToWString(), L'?');
        if (level == cef_log_severity_t::LOGSEVERITY_DEBUG)
            CASPAR_LOG(debug) << print() << L" Log: " << msg;
        else if (level == cef_log_severity_t::LOGSEVERITY_WARNING)
            CASPAR_LOG(warning) << print() << L" Log: " << msg;
        else if (level == cef_log_severity_t::LOGSEVERITY_ERROR)
            CASPAR_LOG(error) << print() << L" Log: " << msg;
        else if (level == cef_log_severity_t::LOGSEVERITY_FATAL)
            CASPAR_LOG(fatal) << print() << L" Log: " << msg;
        else
            CASPAR_LOG(info) << print() << L" Log: " << msg;
        return true;
    }

    CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }

    CefRefPtr<CefAudioHandler> GetAudioHandler() override { return this; }

    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }

    CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }

    CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }

    CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }

    static constexpr int    max_renderer_reloads       = 5;
    static constexpr double renderer_crash_window_secs = 60.0;

    void OnRenderProcessTerminated(CefRefPtr<CefBrowser> browser,
                                   TerminationStatus     status,
                                   int                    error_code,
                                   const CefString&       error_string) override
    {
        if (closing_)
            return;

        const auto now = std::chrono::steady_clock::now();
        while (!renderer_termination_times_.empty() &&
               std::chrono::duration<double>(now - renderer_termination_times_.front()).count() >=
                   renderer_crash_window_secs)
            renderer_termination_times_.pop_front();

        {
            std::lock_guard<std::mutex> lock(javascript_mutex_);
            if (recovery_stopped_)
                return;

            loaded_ = false;
            if (renderer_termination_times_.size() >= max_renderer_reloads) {
                recovery_stopped_ = true;
                std::wstring javascript;
                while (javascript_before_load_.try_pop(javascript)) {
                }
                CASPAR_LOG(error) << "[html_producer] " << print() << L" renderer process terminated (status: "
                                  << static_cast<int>(status) << L"); already reloaded after " << max_renderer_reloads
                                  << L" terminations within " << static_cast<int>(renderer_crash_window_secs)
                                  << L"s; giving up on automatic reload and discarding pending JavaScript. "
                                  << L"Further JavaScript commands will fail. Manual RELOAD or REMOVE/LOAD is required.";
                return;
            }
        }

        renderer_termination_times_.push_back(now);
        CASPAR_LOG(warning) << "[html_producer] " << print() << L" renderer process terminated (status: "
                            << static_cast<int>(status) << L"), reloading.";
        browser->Reload();
    }

    void OnLoadError(CefRefPtr<CefBrowser> browser,
                     CefRefPtr<CefFrame>   frame,
                     ErrorCode             errorCode,
                     const CefString&      errorText,
                     const CefString&      failedUrl) override
    {
        // Replacing a template normally cancels its in-flight navigation.
        // Neither cancellation nor a failed child iframe invalidates the
        // parent renderer and its independent on-air graphics.
        if (errorCode == ERR_ABORTED)
            return;
        CASPAR_LOG(warning) << "[html_producer] " << errorText.ToString() << " while loading url: \""
                            << failedUrl.ToString() << "\"";

        if (!frame->IsMain())
            return;
        not_found_ = true;
        // Stop producing if the page fails to load
        {
            std::lock_guard<std::mutex> lock(frames_mutex_);
            frames_.push(presentation_frame());
        }

        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            state_ = {};
        }
    }

    void OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int httpStatusCode) override
    {
        if (!frame->IsMain())
            return;

        std::lock_guard<std::mutex> lock(javascript_mutex_);
        if (not_found_ || recovery_stopped_)
            return;

        loaded_ = true;
        execute_queued_javascript();
    }

    bool OnProcessMessageReceived(CefRefPtr<CefBrowser>        browser,
                                  CefRefPtr<CefFrame>          frame,
                                  CefProcessId                 source_process,
                                  CefRefPtr<CefProcessMessage> message) override
    {
        auto name = message->GetName().ToString();

        if (name == REMOVE_MESSAGE_NAME) {
            // TODO fully remove producer
            this->close();

            {
                std::lock_guard<std::mutex> lock(frames_mutex_);
                frames_.push(presentation_frame());
            }

            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                state_ = {};
            }

            return true;
        }
        if (name == LOG_MESSAGE_NAME) {
            auto args     = message->GetArgumentList();
            auto severity = static_cast<boost::log::trivial::severity_level>(args->GetInt(0));
            auto msg      = log::replace_nonprintable_copy(args->GetString(1).ToWString(), L'?');

            BOOST_LOG_SEV(log::logger::get(), severity) << print() << L" [renderer_process] " << msg;
        }

        return false;
    }

    bool GetAudioParameters(CefRefPtr<CefBrowser> browser, CefAudioParameters& params) override
    {
        params.channel_layout    = CEF_CHANNEL_LAYOUT_7_1;
        params.sample_rate       = format_desc_.audio_sample_rate;
        params.frames_per_buffer = format_desc_.audio_cadence[0];
        return format_desc_.audio_cadence.size() == 1; // TODO - handle 59.94
    }

    void OnAudioStreamStarted(CefRefPtr<CefBrowser> browser, const CefAudioParameters& params, int channels) override
    {
        audioResampler_ = std::make_unique<ffmpeg::AudioResampler>(params.sample_rate, AV_SAMPLE_FMT_FLTP);
    }
    void OnAudioStreamPacket(CefRefPtr<CefBrowser> browser, const float** data, int samples, int64_t pts) override
    {
        if (!audioResampler_)
            return;

        auto audio       = audioResampler_->convert(samples, reinterpret_cast<const void**>(data));
        auto audio_frame = core::mutable_frame(this, {}, std::move(audio), core::pixel_format_desc());

        {
            std::lock_guard<std::mutex> lock(audio_frames_mutex_);
            while (audio_frames_.size() >= frames_max_size_) {
                audio_frames_.pop();
            }
            audio_frames_.push(presentation_frame(core::draw_frame(std::move(audio_frame))));
        }
    }
    void OnAudioStreamStopped(CefRefPtr<CefBrowser> browser) override { audioResampler_ = nullptr; }
    void OnAudioStreamError(CefRefPtr<CefBrowser> browser, const CefString& message) override
    {
        CASPAR_LOG(info) << "[html_producer] OnAudioStreamError: \"" << message.ToString() << "\"";
        audioResampler_ = nullptr;
    }

    void do_execute_javascript(const std::wstring& javascript)
    {
        html::begin_invoke([this, javascript] {
            if (browser_ != nullptr)
                browser_->GetMainFrame()->ExecuteJavaScript(
                    u8(javascript).c_str(), browser_->GetMainFrame()->GetURL(), 0);
        });
    }

    void execute_queued_javascript()
    {
        std::wstring javascript;

        while (javascript_before_load_.try_pop(javascript))
            do_execute_javascript(javascript);
    }

    std::wstring print() const
    {
        return L"html[" + url_ + L"]" + L" " + std::to_wstring(format_desc_.square_width) + L" " +
               std::to_wstring(format_desc_.square_height) + L" " + std::to_wstring(format_desc_.fps);
    }

    IMPLEMENT_REFCOUNTING(html_client);
};

class html_producer : public core::frame_producer
{
    core::video_format_desc             format_desc_;
    const std::wstring                  url_;
    spl::shared_ptr<diagnostics::graph> graph_;

    CefRefPtr<html_client> client_;

  public:
    html_producer(const spl::shared_ptr<core::frame_factory>& frame_factory,
                  const core::video_format_desc&              format_desc,
                  const std::wstring&                         url)
        : format_desc_(format_desc)
        , url_(url)
    {
        html::invoke([&] {
            auto gpu = is_gpu_shared_texture_enabled();

            client_ = new html_client(frame_factory, graph_, format_desc, gpu.first, gpu.second, url_);

            CefWindowInfo window_info;
            window_info.bounds.width                 = format_desc.square_width;
            window_info.bounds.height                = format_desc.square_height;
            window_info.windowless_rendering_enabled = true;
            window_info.shared_texture_enabled       = gpu.second;

            CefBrowserSettings browser_settings;
            browser_settings.webgl = gpu.first ? cef_state_t::STATE_ENABLED : cef_state_t::STATE_DISABLED;
            double fps             = format_desc.fps;
            browser_settings.windowless_frame_rate = int(ceil(fps));
            CefBrowserHost::CreateBrowser(window_info, client_.get(), url, browser_settings, nullptr, nullptr);
        });
    }

    ~html_producer() override
    {
        if (client_ != nullptr)
            client_->close();
    }

    // frame_producer

    std::wstring name() const override { return L"html"; }

    core::draw_frame receive_impl(const core::video_field field, int nb_samples) override
    {
        if (client_ != nullptr) {
            return client_->receive(field);
        }

        return core::draw_frame::empty();
    }

    core::draw_frame first_frame(const core::video_field field) override { return receive_impl(field, 0); }

    bool is_ready() override
    {
        if (client_ != nullptr) {
            return client_->is_ready();
        }
        return false;
    }

    core::draw_frame last_frame(const core::video_field field) override
    {
        if (client_ != nullptr) {
            return client_->last_frame();
        }

        return core::draw_frame::empty();
    }

    std::future<std::wstring> call(const std::vector<std::wstring>& params) override
    {
        if (client_ == nullptr)
            return make_ready_future(std::wstring());

        auto javascript = params.at(0);

        if (javascript == L"RELOAD") {
            client_->reload();
        } else {
            client_->execute_javascript(javascript);
        }

        return make_ready_future(std::wstring());
    }

    std::wstring print() const override { return L"html[" + url_ + L"]"; }

    core::monitor::state state() const override
    {
        if (client_ != nullptr) {
            return client_->state();
        }

        static const core::monitor::state empty;
        return empty;
    }
};

spl::shared_ptr<core::frame_producer> create_cg_producer(const core::frame_producer_dependencies& dependencies,
                                                         const std::vector<std::wstring>&         params)
{
    const auto html_prefix    = boost::iequals(params.at(0), L"[HTML]");
    const auto param_url      = html_prefix ? params.at(1) : params.at(0);
    const auto filename       = env::template_folder() + param_url + L".html";
    const auto found_filename = find_case_insensitive(filename);
    const auto http_prefix =
        boost::algorithm::istarts_with(param_url, L"http:") || boost::algorithm::istarts_with(param_url, L"https:");

    if (!found_filename && !http_prefix && !html_prefix)
        return core::frame_producer::empty();

    const auto url = found_filename ? L"file://" + *found_filename : param_url;

    std::optional<int> width;
    std::optional<int> height;
    {
        auto u8_url = u8(url);

        boost::smatch what;
        if (boost::regex_search(u8_url, what, boost::regex("width=([0-9]+)"))) {
            width = std::stoi(what[1].str());
        }

        if (boost::regex_search(u8_url, what, boost::regex("height=([0-9]+)"))) {
            height = std::stoi(what[1].str());
        }
    }

    auto format_desc = dependencies.format_desc;
    if (width && height) {
        format_desc.width         = *width;
        format_desc.square_width  = *width;
        format_desc.height        = *height;
        format_desc.square_height = *height;
    }

    return spl::make_shared<html_producer>(dependencies.frame_factory, format_desc, url);
}

spl::shared_ptr<core::frame_producer> create_producer(const core::frame_producer_dependencies& dependencies,
                                                      const std::vector<std::wstring>&         params)
{
    return create_cg_producer(dependencies, params);
}

}} // namespace caspar::html
