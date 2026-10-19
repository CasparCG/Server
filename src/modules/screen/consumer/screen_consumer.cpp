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

#include "screen_consumer.h"

#include <GL/glew.h>
#include <SFML/Window.hpp>

#include <common/array.h>
#include <common/diagnostics/graph.h>
#include <common/future.h>
#include <common/gl/gl_check.h>
#include <common/log.h>
#include <common/memory.h>
#include <common/param.h>
#include <common/timer.h>
#include <common/timespan.h>
#include <common/utf.h>

#include <core/consumer/channel_info.h>
#include <core/consumer/frame_consumer.h>
#include <core/frame/frame.h>
#include <core/frame/geometry.h>
#include <core/video_format.h>

#include <boost/algorithm/string.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/property_tree/ptree.hpp>

#include <tbb/concurrent_queue.h>

#include <thread>
#include <tuple> // std::ignore
#include <utility>
#include <vector>

#if defined(_MSC_VER)
#include <windows.h>
#else
#include "../util/x11_util.h"
#endif

#include "consumer_screen_fragment.h"
#include "consumer_screen_vertex.h"
#include <accelerator/ogl/util/shader.h>

namespace caspar { namespace screen {

std::unique_ptr<accelerator::ogl::shader> get_shader()
{
    return std::make_unique<accelerator::ogl::shader>(std::string(reinterpret_cast<const char*>(vertex_shader)),
                                                      std::string(reinterpret_cast<const char*>(fragment_shader)));
}

enum class stretch
{
    none,
    uniform,
    fill,
    uniform_to_fill
};

struct configuration
{
    enum class aspect_ratio
    {
        aspect_4_3 = 0,
        aspect_16_9,
        aspect_invalid,
    };

    enum class colour_spaces
    {
        RGB               = 0,
        datavideo_full    = 1,
        datavideo_limited = 2
    };

    std::wstring    name          = L"Screen consumer";
    int             screen_index  = 0;
    int             screen_x      = 0;
    int             screen_y      = 0;
    int             screen_width  = 0;
    int             screen_height = 0;
    screen::stretch stretch       = screen::stretch::fill;
    bool            windowed      = true;
    bool            key_only      = false;
    bool            sbs_key       = false;
    aspect_ratio    aspect        = aspect_ratio::aspect_invalid;
    bool            vsync         = false;
    bool            interactive   = true;
    bool            borderless    = false;
    bool            always_on_top = false;
    colour_spaces   colour_space  = colour_spaces::RGB;
    bool            high_bitdepth = false;
    bool            gpu_texture   = false;

    timespan        delay;
};

struct frame
{
    GLuint                         pbo = 0;
    GLuint                         tex = 0;
    char*                          ptr = nullptr;
    std::shared_ptr<core::texture> texture;
    GLsync                         fence = nullptr;
};

struct screen_consumer;

struct display_strategy
{
    virtual ~display_strategy() {}
    virtual frame init_frame(const configuration& config, const core::video_format_desc& format_desc) = 0;
    virtual void  cleanup_frame(frame& frame)                                                         = 0;
    virtual void  do_tick(screen_consumer* self)                                                      = 0;
};
struct gpu_strategy;
struct host_strategy;

struct screen_consumer
{
    const configuration     config_;
    core::video_format_desc format_desc_;
    int                     channel_index_;

    std::vector<frame> frames_;

    int screen_width_  = format_desc_.width;
    int screen_height_ = format_desc_.height;
    int square_width_  = format_desc_.square_width;
    int square_height_ = format_desc_.square_height;
    int screen_x_      = 0;
    int screen_y_      = 0;

    std::vector<core::frame_geometry::coord> draw_coords_;

    sf::Window window_;

    spl::shared_ptr<diagnostics::graph> graph_;
    caspar::timer                       tick_timer_;

    tbb::concurrent_bounded_queue<core::const_frame> frame_buffer_;

    std::unique_ptr<accelerator::ogl::shader> shader_;
    GLuint                                    vao_;
    GLuint                                    vbo_;

    std::atomic<bool> is_running_{true};
    std::thread       thread_;

    spl::shared_ptr<display_strategy> strategy_;

    screen_consumer(const screen_consumer&)            = delete;
    screen_consumer& operator=(const screen_consumer&) = delete;

  public:
    screen_consumer(const configuration& config, const core::video_format_desc& format_desc, int channel_index)
        : config_(config)
        , format_desc_(format_desc)
        , channel_index_(channel_index)
        , strategy_(config.gpu_texture ? spl::make_shared<display_strategy, gpu_strategy>()
                                       : spl::make_shared<display_strategy, host_strategy>())
    {
        if (config_.gpu_texture) {
            CASPAR_LOG(info) << print() << " Using GPU texture for rendering.";
        } else {
            CASPAR_LOG(info) << print() << " Using frame copied to host for rendering.";
        }

        if (format_desc_.format == core::video_format::ntsc &&
            config_.aspect == configuration::aspect_ratio::aspect_4_3) {
            // Use default values which are 4:3.
        } else {
            if (config_.aspect == configuration::aspect_ratio::aspect_16_9) {
                square_width_ = format_desc.height * 16 / 9;
            } else if (config_.aspect == configuration::aspect_ratio::aspect_4_3) {
                square_width_ = format_desc.height * 4 / 3;
            }
        }

        auto delay_frames = config_.delay.in_frames(format_desc_.fps);
        delay_frames = std::clamp<int>(delay_frames, 0, format_desc_.fps);
        CASPAR_LOG(info) << print() << " Latency: " << delay_frames << " frames";

        frame_buffer_.set_capacity(delay_frames + 1);
        while (delay_frames--) frame_buffer_.push(core::const_frame{});

        graph_->set_color("tick-time", diagnostics::color(0.0f, 0.6f, 0.9f));
        graph_->set_color("frame-time", diagnostics::color(0.1f, 1.0f, 0.1f));
        graph_->set_color("dropped-frame", diagnostics::color(0.3f, 0.6f, 0.3f));
        graph_->set_text(print());
        diagnostics::register_graph(graph_);

#if defined(_MSC_VER)
        DISPLAY_DEVICE              d_device = {sizeof(d_device), 0};
        std::vector<DISPLAY_DEVICE> displayDevices;
        for (int n = 0; EnumDisplayDevices(nullptr, n, &d_device, NULL); ++n) {
            displayDevices.push_back(d_device);
        }

        if (config_.screen_index >= displayDevices.size()) {
            CASPAR_LOG(warning) << print() << L" Invalid screen-index: " << config_.screen_index;
        }

        DEVMODE devmode = {};
        if (!EnumDisplaySettings(displayDevices[config_.screen_index].DeviceName, ENUM_CURRENT_SETTINGS, &devmode)) {
            CASPAR_LOG(warning) << print() << L" Could not find display settings for screen-index: "
                                << config_.screen_index;
        }

        screen_x_      = devmode.dmPosition.x;
        screen_y_      = devmode.dmPosition.y;
        screen_width_  = devmode.dmPelsWidth;
        screen_height_ = devmode.dmPelsHeight;
#else
        if (config_.screen_index > 1) {
            CASPAR_LOG(warning) << print() << L" Screen-index is not supported on linux";
        }
#endif

        if (config.windowed) {
            screen_x_ += config.screen_x;
            screen_y_ += config.screen_y;

            if (config.screen_width > 0 && config.screen_height > 0) {
                screen_width_  = config.screen_width;
                screen_height_ = config.screen_height;
            } else if (config.screen_width > 0) {
                screen_width_  = config.screen_width;
                screen_height_ = square_height_ * config.screen_width / square_width_;
            } else if (config.screen_height > 0) {
                screen_height_ = config.screen_height;
                screen_width_  = square_width_ * config.screen_height / square_height_;
            } else {
                screen_width_  = square_width_;
                screen_height_ = square_height_;
            }
        }

        thread_ = std::thread([this] {
            try {
#if SFML_VERSION_MAJOR >= 3
                sf::VideoMode mode{sf::Vector2u(config_.sbs_key ? screen_width_ * 2 : screen_width_, screen_height_),
                                   sf::VideoMode::getDesktopMode().bitsPerPixel};
                sf::ContextSettings settings{.depthBits         = 0,
                                             .stencilBits       = 0,
                                             .antiAliasingLevel = 0,
                                             .majorVersion      = 4,
                                             .minorVersion      = 5,
                                             .attributeFlags    = sf::ContextSettings::Attribute::Core};
                auto state = config_.windowed || config_.borderless ? sf::State::Windowed : sf::State::Fullscreen;
                auto style = config_.borderless ? sf::Style::None : sf::Style::Default;
                window_.create(mode, u8(print()), style, state, settings);
#else
                const auto    window_style = config_.borderless ? sf::Style::None
                                             : config_.windowed ? sf::Style::Resize | sf::Style::Close
                                                                : sf::Style::Fullscreen;
                sf::VideoMode desktop      = sf::VideoMode::getDesktopMode();
                sf::VideoMode mode(
                    config_.sbs_key ? screen_width_ * 2 : screen_width_, screen_height_, desktop.bitsPerPixel);
                window_.create(mode,
                               u8(print()),
                               window_style,
                               sf::ContextSettings(0, 0, 0, 4, 5, sf::ContextSettings::Attribute::Core));
#endif
                window_.setPosition(sf::Vector2i(screen_x_, screen_y_));
                window_.setMouseCursorVisible(config_.interactive);
                std::ignore = window_.setActive(true);

                if (config_.always_on_top) {
#ifdef _MSC_VER
                    HWND hwnd = window_.getSystemHandle();
                    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
#else
                    window_always_on_top(window_);
#endif
                }

                if (glewInit() != GLEW_OK) {
                    CASPAR_THROW_EXCEPTION(gl::ogl_exception() << msg_info("Failed to initialize GLEW."));
                }

                if (!GLEW_VERSION_4_5 && (glewIsSupported("GL_ARB_sync GL_ARB_shader_objects GL_ARB_multitexture "
                                                          "GL_ARB_direct_state_access GL_ARB_texture_barrier") == 0u)) {
                    CASPAR_THROW_EXCEPTION(not_supported() << msg_info(
                                               "Your graphics card does not meet the minimum hardware requirements "
                                               "since it does not support OpenGL 4.5 or higher."));
                }

                GL(glGenVertexArrays(1, &vao_));
                GL(glGenBuffers(1, &vbo_));
                GL(glBindVertexArray(vao_));
                GL(glBindBuffer(GL_ARRAY_BUFFER, vbo_));

                shader_ = get_shader();
                shader_->use();
                shader_->set("background", 0);
                shader_->set("window_width", screen_width_);

                for (int n = 0; n < 2; ++n) {
                    frames_.push_back(strategy_->init_frame(config_, format_desc_));
                }

                GL(glDisable(GL_DEPTH_TEST));
                GL(glClearColor(0.0, 0.0, 0.0, 0.0));
                GL(glViewport(
                    0, 0, config_.sbs_key ? format_desc_.width * 2 : format_desc_.width, format_desc_.height));

                calculate_aspect();

                window_.setVerticalSyncEnabled(config_.vsync);
                if (config_.vsync) {
                    CASPAR_LOG(info) << print() << " Enabled vsync.";
                }

                shader_->set("colour_space", config_.colour_space);
                if (config_.colour_space == configuration::colour_spaces::datavideo_full ||
                    config_.colour_space == configuration::colour_spaces::datavideo_limited) {
                    CASPAR_LOG(info) << print() << " Enabled colours conversion for DataVideo TC-100/TC-200 "
                                     << (config_.colour_space == configuration::colour_spaces::datavideo_full
                                             ? "(Full Range)."
                                             : "(Limited Range).");
                }

                glClear(GL_COLOR_BUFFER_BIT);
                window_.display();

                while (is_running_) {
                    tick();
                }
            } catch (tbb::user_abort&) {
                // Do nothing
            } catch (...) {
                CASPAR_LOG_CURRENT_EXCEPTION();
                is_running_ = false;
            }

            for (auto frame : frames_) {
                strategy_->cleanup_frame(frame);
            }

            shader_.reset();
            GL(glDeleteVertexArrays(1, &vao_));
            GL(glDeleteBuffers(1, &vbo_));

            window_.close();
        });
    }

    ~screen_consumer()
    {
        is_running_ = false;
        frame_buffer_.abort();
        thread_.join();
    }

    bool poll()
    {
        int count = 0;
#if SFML_VERSION_MAJOR >= 3
        while (const auto e = window_.pollEvent()) {
            count++;
            if (e->is<sf::Event::Resized>()) {
                calculate_aspect();
            } else if (e->is<sf::Event::Closed>()) {
                is_running_ = false;
            }
        }
#else
        sf::Event e;
        while (window_.pollEvent(e)) {
            count++;
            if (e.type == sf::Event::Resized) {
                calculate_aspect();
            } else if (e.type == sf::Event::Closed) {
                is_running_ = false;
            }
        }
#endif
        return count > 0;
    }

    void tick()
    {
        strategy_->do_tick(this);

        window_.display();

        std::rotate(frames_.begin(), frames_.begin() + 1, frames_.end());

        graph_->set_value("tick-time", tick_timer_.elapsed() * format_desc_.fps * 0.5);
        tick_timer_.restart();
    }

    void draw()
    {
        GL(glBufferData(GL_ARRAY_BUFFER,
                        static_cast<GLsizeiptr>(sizeof(core::frame_geometry::coord)) * draw_coords_.size(),
                        draw_coords_.data(),
                        GL_STATIC_DRAW));

        auto stride = static_cast<GLsizei>(sizeof(core::frame_geometry::coord));

        auto vtx_loc = shader_->get_attrib_location("Position");
        auto tex_loc = shader_->get_attrib_location("TexCoordIn");

        GL(glEnableVertexAttribArray(vtx_loc));
        GL(glEnableVertexAttribArray(tex_loc));

        GL(glVertexAttribPointer(vtx_loc, 2, GL_DOUBLE, GL_FALSE, stride, nullptr));
        GL(glVertexAttribPointer(tex_loc, 4, GL_DOUBLE, GL_FALSE, stride, (GLvoid*)(2 * sizeof(GLdouble))));

        shader_->set("window_width", screen_width_);

        if (config_.sbs_key) {
            auto coords_size = static_cast<GLsizei>(draw_coords_.size());

            // First half fill
            shader_->set("key_only", false);
            GL(glDrawArrays(GL_TRIANGLES, 0, coords_size / 2));

            // Second half key
            shader_->set("key_only", true);
            GL(glDrawArrays(GL_TRIANGLES, coords_size / 2, coords_size / 2));
        } else {
            shader_->set("key_only", config_.key_only);
            GL(glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(draw_coords_.size())));
        }

        GL(glDisableVertexAttribArray(vtx_loc));
        GL(glDisableVertexAttribArray(tex_loc));
        GL(glBindTexture(GL_TEXTURE_2D, 0));
    }

    std::future<bool> send(core::video_field field, const core::const_frame& frame)
    {
        const int MAX_TRIES = 3;
        int       count     = 0;
        while (count++ < MAX_TRIES && is_running_) {
            if (frame_buffer_.try_push(frame))
                break;

            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        if (count == MAX_TRIES) {
            graph_->set_tag(diagnostics::tag_severity::WARNING, "dropped-frame");
        }

        return make_ready_future(is_running_.load());
    }

    std::wstring channel_and_format() const
    {
        return L"[" + std::to_wstring(channel_index_) + L"|" + format_desc_.name + L"]";
    }

    std::wstring print() const { return config_.name + L" " + channel_and_format(); }

    void calculate_aspect()
    {
        if (config_.windowed) {
            screen_height_ = window_.getSize().y;
            screen_width_  = window_.getSize().x;
        }

        GL(glViewport(0, 0, screen_width_, screen_height_));

        std::pair<float, float> target_ratio = none();
        if (config_.stretch == screen::stretch::fill) {
            target_ratio = Fill();
        } else if (config_.stretch == screen::stretch::uniform) {
            target_ratio = uniform();
        } else if (config_.stretch == screen::stretch::uniform_to_fill) {
            target_ratio = uniform_to_fill();
        }

        if (config_.sbs_key) {
            draw_coords_ = {
                // First half fill
                {-target_ratio.first, target_ratio.second, 0.0, 0.0}, // upper left
                {0, target_ratio.second, 1.0, 0.0},                   // upper right
                {0, -target_ratio.second, 1.0, 1.0},                  // lower right

                {-target_ratio.first, target_ratio.second, 0.0, 0.0},  // upper left
                {0, -target_ratio.second, 1.0, 1.0},                   // lower right
                {-target_ratio.first, -target_ratio.second, 0.0, 1.0}, // lower left

                // Second half key
                {0, target_ratio.second, 0.0, 0.0},                   // upper left
                {target_ratio.first, target_ratio.second, 1.0, 0.0},  // upper right
                {target_ratio.first, -target_ratio.second, 1.0, 1.0}, // lower right

                {0, target_ratio.second, 0.0, 0.0},                   // upper left
                {target_ratio.first, -target_ratio.second, 1.0, 1.0}, // lower right
                {0, -target_ratio.second, 0.0, 1.0}                   // lower left
            };
        } else {
            draw_coords_ = {
                //    vertex    texture
                {-target_ratio.first, target_ratio.second, 0.0, 0.0}, // upper left
                {target_ratio.first, target_ratio.second, 1.0, 0.0},  // upper right
                {target_ratio.first, -target_ratio.second, 1.0, 1.0}, // lower right

                {-target_ratio.first, target_ratio.second, 0.0, 0.0}, // upper left
                {target_ratio.first, -target_ratio.second, 1.0, 1.0}, // lower right
                {-target_ratio.first, -target_ratio.second, 0.0, 1.0} // lower left
            };
        }
    }

    std::pair<float, float> none() const
    {
        float width =
            static_cast<float>(config_.sbs_key ? square_width_ * 2 : square_width_) / static_cast<float>(screen_width_);
        float height = static_cast<float>(square_height_) / static_cast<float>(screen_height_);

        return std::make_pair(width, height);
    }

    std::pair<float, float> uniform() const
    {
        float aspect = static_cast<float>(config_.sbs_key ? square_width_ * 2 : square_width_) /
                       static_cast<float>(square_height_);
        float width  = std::min(1.0f, static_cast<float>(screen_height_) * aspect / static_cast<float>(screen_width_));
        float height = static_cast<float>(screen_width_ * width) / static_cast<float>(screen_height_ * aspect);

        return std::make_pair(width, height);
    }

    static std::pair<float, float> Fill() { return std::make_pair(1.0f, 1.0f); }

    std::pair<float, float> uniform_to_fill() const
    {
        float wr =
            static_cast<float>(config_.sbs_key ? square_width_ * 2 : square_width_) / static_cast<float>(screen_width_);
        float hr    = static_cast<float>(square_height_) / static_cast<float>(screen_height_);
        float r_inv = 1.0f / std::min(wr, hr);

        float width  = wr * r_inv;
        float height = hr * r_inv;

        return std::make_pair(width, height);
    }
};

struct host_strategy : public display_strategy
{
    virtual ~host_strategy() {}

    virtual frame init_frame(const configuration& config, const core::video_format_desc& format_desc) override
    {
        screen::frame frame;
        auto          flags = GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT | GL_MAP_WRITE_BIT;
        GL(glCreateBuffers(1, &frame.pbo));
        auto size_multiplier = config.high_bitdepth ? 2 : 1;
        GL(glNamedBufferStorage(frame.pbo, format_desc.size * size_multiplier, nullptr, flags));
        frame.ptr = reinterpret_cast<char*>(
            GL2(glMapNamedBufferRange(frame.pbo, 0, format_desc.size * size_multiplier, flags)));

        GL(glCreateTextures(GL_TEXTURE_2D, 1, &frame.tex));
        GL(glTextureParameteri(frame.tex,
                               GL_TEXTURE_MIN_FILTER,
                               (config.colour_space == configuration::colour_spaces::datavideo_full ||
                                config.colour_space == configuration::colour_spaces::datavideo_limited)
                                   ? GL_NEAREST
                                   : GL_LINEAR));
        GL(glTextureParameteri(frame.tex,
                               GL_TEXTURE_MAG_FILTER,
                               (config.colour_space == configuration::colour_spaces::datavideo_full ||
                                config.colour_space == configuration::colour_spaces::datavideo_limited)
                                   ? GL_NEAREST
                                   : GL_LINEAR));
        GL(glTextureParameteri(frame.tex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
        GL(glTextureParameteri(frame.tex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
        GL(glTextureStorage2D(
            frame.tex, 1, config.high_bitdepth ? GL_RGBA16 : GL_RGBA8, format_desc.width, format_desc.height));
        GL(glClearTexImage(
            frame.tex, 0, GL_BGRA, config.high_bitdepth ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE, nullptr));

        return frame;
    }

    virtual void cleanup_frame(frame& frame) override
    {
        GL(glUnmapNamedBuffer(frame.pbo));
        glDeleteBuffers(1, &frame.pbo);
        glDeleteTextures(1, &frame.tex);
    }

    virtual void do_tick(screen_consumer* self) override
    {
        core::const_frame in_frame;

        while (!self->frame_buffer_.try_pop(in_frame) && self->is_running_) {
            // TODO (fix)
            if (!self->poll()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }

        if (!in_frame) {
            return;
        }

        // Upload
        {
            auto& frame = self->frames_.front();

            while (frame.fence != nullptr) {
                auto wait = glClientWaitSync(frame.fence, 0, 0);
                if (wait == GL_ALREADY_SIGNALED || wait == GL_CONDITION_SATISFIED) {
                    glDeleteSync(frame.fence);
                    frame.fence = nullptr;
                }
                if (!self->poll()) {
                    // TODO (fix)
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            }

            auto size_multiplier = self->config_.high_bitdepth ? 2 : 1;
            std::memcpy(frame.ptr, in_frame.image_data(0).begin(), self->format_desc_.size * size_multiplier);

            GL(glBindBuffer(GL_PIXEL_UNPACK_BUFFER, frame.pbo));
            GL(glTextureSubImage2D(frame.tex,
                                   0,
                                   0,
                                   0,
                                   self->format_desc_.width,
                                   self->format_desc_.height,
                                   GL_BGRA,
                                   self->config_.high_bitdepth ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE,
                                   nullptr));
            GL(glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0));

            frame.fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        }

        // Display
        {
            auto& frame = self->frames_.back();

            GL(glClear(GL_COLOR_BUFFER_BIT));

            GL(glActiveTexture(GL_TEXTURE0));
            GL(glBindTexture(GL_TEXTURE_2D, frame.tex));

            self->draw();
        }
    }
};

struct gpu_strategy : public display_strategy
{
    virtual ~gpu_strategy() {}
    virtual frame init_frame(const configuration& config, const core::video_format_desc& format_desc) override
    {
        return frame();
    }
    virtual void cleanup_frame(frame& frame) override
    {
        if (frame.fence) {
            glDeleteSync(frame.fence);
            frame.fence = nullptr;
        }
        frame.texture.reset();
    }

    virtual void do_tick(screen_consumer* self) override
    {
        core::const_frame in_frame;

        self->poll();

        while (!self->frame_buffer_.try_pop(in_frame) && self->is_running_) {
            // TODO (fix)
            if (!self->poll()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }

        // Display
        {
            auto& frame = self->frames_.front();

            while (frame.fence != nullptr && self->is_running_) {
                auto wait = glClientWaitSync(frame.fence, 0, 0);
                if (wait == GL_ALREADY_SIGNALED || wait == GL_CONDITION_SATISFIED) {
                    glDeleteSync(frame.fence);
                    frame.fence = nullptr;
                    frame.texture.reset();
                }

                if (!self->poll()) {
                    // TODO (fix)
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            }

            if (!in_frame || !self->is_running_) {
                self->graph_->set_value("tick-time", self->tick_timer_.elapsed() * self->format_desc_.fps * 0.5);
                self->tick_timer_.restart();
                return;
            }

            GL(glClear(GL_COLOR_BUFFER_BIT));

            if (in_frame.texture()) {
                in_frame.texture()->bind(0);

                self->draw();

                frame.fence   = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
                frame.texture = in_frame.texture();
            }
        }
    }
};

struct screen_consumer_proxy : public core::frame_consumer
{
    const configuration              config_;
    std::unique_ptr<screen_consumer> consumer_;

  public:
    explicit screen_consumer_proxy(configuration config)
        : config_(std::move(config))
    {
    }

    // frame_consumer

    void initialize(const core::video_format_desc& format_desc,
                    const core::channel_info&      channel_info,
                    int                            port_index) override
    {
        consumer_.reset();
        consumer_ = std::make_unique<screen_consumer>(config_, format_desc, channel_info.index);
    }

    std::future<bool> send(core::video_field field, core::const_frame frame) override
    {
        return consumer_->send(field, frame);
    }

    std::wstring print() const override
    {
        auto id_str = consumer_id().empty() ? L"" : L"|id=" + consumer_id();
        return consumer_ ? consumer_->print() + id_str : L"[screen_consumer" + id_str + L"]";
    }

    std::wstring name() const override { return L"screen"; }

    bool has_synchronization_clock() const override { return false; }

    int index() const override { return 600 + (config_.key_only ? 10 : 0) + config_.screen_index; }

    core::monitor::state state() const override
    {
        core::monitor::state state;
        state["screen/name"]          = config_.name;
        state["screen/index"]         = config_.screen_index;
        state["screen/key_only"]      = config_.key_only;
        state["screen/always_on_top"] = config_.always_on_top;
        if (!consumer_id().empty())
            state["screen/consumer_id"] = consumer_id();
        return state;
    }
};

spl::shared_ptr<core::frame_consumer> create_consumer(const std::vector<std::wstring>&     params,
                                                      const core::video_format_repository& format_repository,
                                                      const std::vector<spl::shared_ptr<core::video_channel>>& channels,
                                                      const core::channel_info& channel_info)
{
    if (params.empty() || !boost::iequals(params.at(0), L"SCREEN")) {
        return core::frame_consumer::empty();
    }

    configuration config;

    config.high_bitdepth = (channel_info.depth != common::bit_depth::bit8);

    if (params.size() > 1) {
        try {
            config.screen_index = std::stoi(params.at(1));
        } catch (...) {
        }
    }

    config.windowed    = !contains_param(L"FULLSCREEN", params);
    config.gpu_texture = contains_param(L"GPU", params);
    config.key_only    = contains_param(L"KEY_ONLY", params);
    config.sbs_key     = contains_param(L"SBS_KEY", params);
    config.interactive = !contains_param(L"NON_INTERACTIVE", params);
    config.borderless  = contains_param(L"BORDERLESS", params);

    if (contains_param(L"NAME", params)) {
        config.name = get_param(L"NAME", params);
    }

    if (contains_param(L"X", params)) {
        config.screen_x = get_param(L"X", params, 0);
    }
    if (contains_param(L"Y", params)) {
        config.screen_y = get_param(L"Y", params, 0);
    }
    if (contains_param(L"WIDTH", params)) {
        config.screen_width = get_param(L"WIDTH", params, 0);
    }
    if (contains_param(L"HEIGHT", params)) {
        config.screen_height = get_param(L"HEIGHT", params, 0);
    }

    if (config.sbs_key && config.key_only) {
        CASPAR_LOG(warning) << L" Key-only not supported with configuration of side-by-side fill and key. Ignored.";
        config.key_only = false;
    }

    if (contains_param(L"DELAY", params)) {
        config.delay = timespan{ u8(get_param(L"DELAY", params, L"")) };
    }

    return spl::make_shared<screen_consumer_proxy>(config);
}

spl::shared_ptr<core::frame_consumer>
create_preconfigured_consumer(const boost::property_tree::wptree&                      ptree,
                              const core::video_format_repository&                     format_repository,
                              const std::vector<spl::shared_ptr<core::video_channel>>& channels,
                              const core::channel_info&                                channel_info)
{
    configuration config;

    config.high_bitdepth = (channel_info.depth != common::bit_depth::bit8);

    config.name          = ptree.get(L"name", config.name);
    config.screen_index  = ptree.get(L"device", config.screen_index + 1) - 1;
    config.screen_x      = ptree.get(L"x", config.screen_x);
    config.screen_y      = ptree.get(L"y", config.screen_y);
    config.screen_width  = ptree.get(L"width", config.screen_width);
    config.screen_height = ptree.get(L"height", config.screen_height);
    config.windowed      = ptree.get(L"windowed", config.windowed);
    config.key_only      = ptree.get(L"key-only", config.key_only);
    config.sbs_key       = ptree.get(L"sbs-key", config.sbs_key);
    config.vsync         = ptree.get(L"vsync", config.vsync);
    config.interactive   = ptree.get(L"interactive", config.interactive);
    config.borderless    = ptree.get(L"borderless", config.borderless);
    config.always_on_top = ptree.get(L"always-on-top", config.always_on_top);
    config.gpu_texture   = ptree.get(L"gpu-texture", config.gpu_texture);

    auto colour_space_value = ptree.get(L"colour-space", L"RGB");
    config.colour_space     = configuration::colour_spaces::RGB;
    if (colour_space_value == L"datavideo-full")
        config.colour_space = configuration::colour_spaces::datavideo_full;
    else if (colour_space_value == L"datavideo-limited")
        config.colour_space = configuration::colour_spaces::datavideo_limited;

    if (config.sbs_key && config.key_only) {
        CASPAR_LOG(warning) << L" Key-only not supported with configuration of side-by-side fill and key. Ignored.";
        config.key_only = false;
    }

    if ((config.colour_space == configuration::colour_spaces::datavideo_full ||
         config.colour_space == configuration::colour_spaces::datavideo_limited) &&
        config.sbs_key) {
        CASPAR_LOG(warning) << L" Side-by-side fill and key not supported for DataVideo TC100/TC200. Ignored.";
        config.sbs_key = false;
    }

    if ((config.colour_space == configuration::colour_spaces::datavideo_full ||
         config.colour_space == configuration::colour_spaces::datavideo_limited) &&
        config.key_only) {
        CASPAR_LOG(warning) << L" Key only not supported for DataVideo TC100/TC200. Ignored.";
        config.key_only = false;
    }

    auto stretch_str = ptree.get(L"stretch", L"fill");
    if (stretch_str == L"none") {
        config.stretch = screen::stretch::none;
    } else if (stretch_str == L"uniform") {
        config.stretch = screen::stretch::uniform;
    } else if (stretch_str == L"uniform_to_fill") {
        config.stretch = screen::stretch::uniform_to_fill;
    }

    auto aspect_str = ptree.get(L"aspect-ratio", L"default");
    if (aspect_str == L"16:9") {
        config.aspect = configuration::aspect_ratio::aspect_16_9;
    } else if (aspect_str == L"4:3") {
        config.aspect = configuration::aspect_ratio::aspect_4_3;
    }

    config.delay = timespan{ u8(ptree.get(L"delay", L"0")) };

    return spl::make_shared<screen_consumer_proxy>(config);
}

}} // namespace caspar::screen
