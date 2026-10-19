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
 * Author: Eliyah Sundström eliyah@sundstroem.com
 */

#include "artnet_consumer.h"

#undef NOMINMAX
// ^^ This is needed to avoid a conflict between boost asio and other header files defining NOMINMAX

#include <common/future.h>
#include <common/log.h>
#include <common/ptree.h>

#include <core/consumer/channel_info.h>
#include <core/frame/frame.h>
#include <core/video_format.h>

#include <boost/algorithm/string.hpp>
#include <boost/asio.hpp>
#include <boost/property_tree/ptree.hpp>

extern "C" {
#define __STDC_CONSTANT_MACROS
#define __STDC_LIMIT_MACROS
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace boost::asio;
using namespace boost::asio::ip;

namespace caspar { namespace artnet {

static const float PI     = 3.141592653589793f;
static const float TWO_PI = 6.283185307179586f;
static const int ROTATION_SUPERSAMPLE = 4;

struct configuration
{
    int refreshRate = 10;

    std::vector<fixture> fixtures;
};

struct output_universe
{
    std::string                   host;
    unsigned short                port;
    int                           universe;
    udp::endpoint                 endpoint;
    std::array<std::uint8_t, 512> buffer{};
};

struct sub_fixture
{
    FixtureType    type;
    int            output_index; // index into outputs_
    unsigned short address;      // 0-based channel within the universe (0..511)
};

// One group corresponds to one <fixture> in config: a cols x rows grid of sub-fixtures
// sharing a box. sws_scale produces a cols x rows BGRA image in `output`, one pixel per
// sub-fixture in row-major order.
struct computed_fixture_group
{
    box                         source_box{};
    fixture_flux                flux;
    int                         cols   = 1;
    int                         rows   = 1;
    int                         crop_x = 0;
    int                         crop_y = 0;
    int                         crop_w = 0;
    int                         crop_h = 0;
    std::shared_ptr<SwsContext> sws;
    std::vector<std::uint8_t>   output;
    std::vector<sub_fixture>    sub_fixtures;

    float brightness = 1.0f;
    float rotation   = 0.0f; // radians
    bool  mirror_x   = false;
    bool  mirror_y   = false;
    bool  rotated    = false;
};

struct artnet_consumer : public core::frame_consumer
{
    const configuration                 config;
    std::vector<computed_fixture_group> fixture_groups;
    std::vector<output_universe>        outputs_;

  public:
    explicit artnet_consumer(configuration config)
        : config(std::move(config))
        , io_context_()
        , socket(io_context_)
    {
        socket.open(udp::v4());

        build_fixture_groups();
    }

    void initialize(const core::video_format_desc& format_desc,
                    const core::channel_info& /*channel_info*/,
                    int /*port_index*/) override
    {
        src_width_    = format_desc.width;
        src_height_   = format_desc.height;
        src_linesize_ = src_width_ * 4;

        for (auto& group : fixture_groups)
            setup_group_sws(group);

        thread_ = std::thread([this] {
            long long time      = 1000 / config.refreshRate;
            auto      last_send = std::chrono::system_clock::now();

            while (!abort_request_) {
                try {
                    auto                          now             = std::chrono::system_clock::now();
                    std::chrono::duration<double> elapsed_seconds = now - last_send;
                    long long                     elapsed_ms =
                        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed_seconds).count();

                    long long sleep_time = time - elapsed_ms;
                    if (sleep_time > 0)
                        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_time));

                    last_send = now;

                    frame_mutex_.lock();
                    auto frame = last_frame_;
                    frame_mutex_.unlock();
                    if (!frame)
                        continue;

                    if ((int)frame.width() != src_width_ || (int)frame.height() != src_height_)
                        continue;

                    for (auto& out : outputs_)
                        std::memset(out.buffer.data(), 0, out.buffer.size());

                    const std::uint8_t* src = frame.image_data(0).data();

                    for (auto& group : fixture_groups) {
                        if (!group.sws)
                            continue;

                        const std::uint8_t* src_ptr;
                        int                 src_stride;
                        int                 src_h;

                        if (group.rotated) {
                            auto [w, h] = rotated_dims(group);
                            fill_rotated_temp(group, src, w, h);
                            src_ptr    = rotated_temp_.data();
                            src_stride = w * 4;
                            src_h      = h;
                        } else {
                            src_ptr    = src + group.crop_y * src_linesize_ + group.crop_x * 4;
                            src_stride = src_linesize_;
                            src_h      = group.crop_h;
                        }

                        const std::uint8_t* sws_src[1]        = {src_ptr};
                        int                 sws_src_stride[1] = {src_stride};
                        std::uint8_t*       sws_dst[1]        = {group.output.data()};
                        int                 sws_dst_stride[1] = {group.cols * 4};
                        sws_scale(group.sws.get(), sws_src, sws_src_stride, 0, src_h, sws_dst, sws_dst_stride);

                        for (size_t i = 0; i < group.sub_fixtures.size(); i++) {
                            const auto& sf  = group.sub_fixtures[i];
                            int         col = (int)(i % (size_t)group.cols);
                            int         row = (int)(i / (size_t)group.cols);

                            // Mirroring is applied in the output grid's space, i.e. after any rotation has
                            // been baked into the resampled image: rotate-then-mirror, not mirror-then-rotate.
                            int src_col = group.mirror_x ? group.cols - 1 - col : col;
                            int src_row = group.mirror_y ? group.rows - 1 - row : row;

                            const std::uint8_t* px = group.output.data() + (src_row * group.cols + src_col) * 4;
                            write_fixture_dmx(sf, px[2], px[1], px[0], group.flux, group.brightness);
                        }
                    }

                    for (auto& out : outputs_)
                        send_dmx_data(out.buffer.data(), 512, out.universe, out.endpoint);
                } catch (...) {
                    CASPAR_LOG_CURRENT_EXCEPTION();
                }
            }
        });
    }

    ~artnet_consumer()
    {
        abort_request_ = true;
        if (thread_.joinable())
            thread_.join();
    }

    std::future<bool> send(core::video_field field, core::const_frame frame) override
    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        last_frame_ = frame;

        return make_ready_future(true);
    }

    std::wstring print() const override { return L"artnet[]"; }

    std::wstring name() const override { return L"artnet"; }

    int index() const override { return 1337; }

    core::monitor::state state() const override
    {
        core::monitor::state state;
        std::size_t          total = 0;
        for (const auto& g : fixture_groups)
            total += g.sub_fixtures.size();
        state["artnet/computed-fixtures"] = total;
        state["artnet/fixtures"]          = config.fixtures.size();
        state["artnet/output-universes"]  = outputs_.size();
        state["artnet/refresh-rate"]      = config.refreshRate;

        return state;
    }

  private:
    core::const_frame last_frame_;
    std::mutex        frame_mutex_;

    std::thread       thread_;
    std::atomic<bool> abort_request_{false};

    int src_width_    = 0;
    int src_height_   = 0;
    int src_linesize_ = 0;

    // Scratch buffer for the rotated path, shared across groups
    std::vector<std::uint8_t> rotated_temp_;

    io_context  io_context_;
    udp::socket socket;

    int output_index_for(const std::wstring& host, unsigned short port, int universe)
    {
        std::string host8 = u8(host);
        for (int i = 0; i < (int)outputs_.size(); i++) {
            const auto& o = outputs_[i];
            if (o.host == host8 && o.port == port && o.universe == universe)
                return i;
        }
        output_universe out{};
        out.host     = host8;
        out.port     = port;
        out.universe = universe;
        out.endpoint = udp::endpoint(make_address(host8), port);
        outputs_.push_back(std::move(out));
        return (int)outputs_.size() - 1;
    }

    void build_fixture_groups()
    {
        fixture_groups.clear();
        outputs_.clear();

        for (const auto& fx : config.fixtures) {
            computed_fixture_group group;
            group.source_box = fx.fixtureBox;
            group.flux       = fx.flux;
            group.brightness = fx.brightness;
            group.cols       = fx.fixtureCols;
            group.rows       = fx.fixtureRows;

            float rot = std::fmod(fx.rotation, TWO_PI);
            if (rot >  PI) rot -= TWO_PI;
            if (rot < -PI) rot += TWO_PI;

            group.rotation = rot;
            group.mirror_x = fx.mirror_x;
            group.mirror_y = fx.mirror_y;
            group.rotated  = std::abs(rot) > 1e-5f;

            int total = (int)fx.fixtureCols * (int)fx.fixtureRows;
            group.sub_fixtures.reserve(total);

            int universe = fx.universe;
            int pos      = fx.startAddress; // 0-based channel within the universe

            for (int i = 0; i < total; i++) {
                // If this sub-fixture would straddle a universe boundary, spill into
                // the next universe (same host/port) and start from channel 0 there.
                if (pos + fx.fixtureChannels > 512) {
                    universe++;
                    pos = 0;
                }

                sub_fixture sf{};
                sf.type         = fx.type;
                sf.output_index = output_index_for(fx.host, fx.port, universe);
                sf.address      = (unsigned short)pos;
                group.sub_fixtures.push_back(sf);

                pos += fx.fixtureChannels;
            }

            if (!group.sub_fixtures.empty())
                fixture_groups.push_back(std::move(group));
        }
    }

    void sample_bilinear(const std::uint8_t* src, float x, float y, std::uint8_t* dst)
    {
        int   left   = (int)std::floor(x);
        int   top    = (int)std::floor(y);
        float frac_x = x - (float)left;
        float frac_y = y - (float)top;

        // A rotated box's corners reach beyond its bounds (and the frame), so extend the edge pixel
        // rather than read out of bounds. This is the sampler's edge behaviour, not box clipping.
        int x0 = std::clamp(left, 0, src_width_ - 1),  x1 = std::clamp(left + 1, 0, src_width_ - 1);
        int y0 = std::clamp(top,  0, src_height_ - 1), y1 = std::clamp(top + 1,  0, src_height_ - 1);

        const std::uint8_t* top_left     = src + y0 * src_linesize_ + x0 * 4;
        const std::uint8_t* top_right    = src + y0 * src_linesize_ + x1 * 4;
        const std::uint8_t* bottom_left  = src + y1 * src_linesize_ + x0 * 4;
        const std::uint8_t* bottom_right = src + y1 * src_linesize_ + x1 * 4;

        const float w_top_left     = (1.0f - frac_x) * (1.0f - frac_y);
        const float w_top_right    = frac_x           * (1.0f - frac_y);
        const float w_bottom_left  = (1.0f - frac_x) * frac_y;
        const float w_bottom_right = frac_x           * frac_y;

        for (int ch = 0; ch < 3; ch++)
            dst[ch] = (std::uint8_t)(top_left[ch] * w_top_left +
                                     top_right[ch] * w_top_right +
                                     bottom_left[ch] * w_bottom_left +
                                     bottom_right[ch] * w_bottom_right +
                                     0.5f);
        dst[3] = 255;
    }

    static std::pair<int, int> rotated_dims(const computed_fixture_group& group)
    {
        int w = std::min(group.cols * ROTATION_SUPERSAMPLE, (int)std::round(group.source_box.width));
        int h = std::min(group.rows * ROTATION_SUPERSAMPLE, (int)std::round(group.source_box.height));
        return {std::max(1, w), std::max(1, h)};
    }

    void fill_rotated_temp(const computed_fixture_group& group, const std::uint8_t* src, int dst_w, int dst_h)
    {
        rotated_temp_.resize((size_t)dst_w * dst_h * 4);
        const box& src_box = group.source_box;

        const float cos_r  = std::cos(group.rotation);
        const float sin_r  = std::sin(group.rotation);
        const float step_x = src_box.width  / (float)dst_w;
        const float step_y = src_box.height / (float)dst_h;

        const float center_x = src_box.x + src_box.width  * 0.5f;
        const float center_y = src_box.y + src_box.height * 0.5f;

        // Walk the grid as an affine map rather than recomputing the rotation per pixel: advancing
        // one column adds a fixed (col_dx, col_dy) to the source sample point. The per-row start is
        // still computed exactly, so float error can't accumulate beyond a single row's width.
        const float col_dx = cos_r * step_x;
        const float col_dy = sin_r * step_x;
        const float off_x0 = 0.5f * step_x - src_box.width * 0.5f; // column-0 offset from center

        std::uint8_t* dst = rotated_temp_.data();
        for (int y = 0; y < dst_h; y++) {
            const float off_y = ((float)y + 0.5f) * step_y - src_box.height * 0.5f;
            float       sample_x = center_x + off_x0 * cos_r - off_y * sin_r;
            float       sample_y = center_y + off_x0 * sin_r + off_y * cos_r;
            for (int x = 0; x < dst_w; x++) {
                sample_bilinear(src, sample_x, sample_y, dst);
                sample_x += col_dx;
                sample_y += col_dy;
                dst += 4;
            }
        }
    }

    void write_fixture_dmx(const sub_fixture& sf,
                           std::uint8_t        r,
                           std::uint8_t        g,
                           std::uint8_t        b,
                           const fixture_flux& flux,
                           float               brightness)
    {
        auto clamp_u8 = [brightness](float v) -> std::uint8_t {
            v = std::round(v * brightness);
            if (v <= 0.0f)
                return 0;
            if (v >= 255.0f)
                return 255;
            return (std::uint8_t)v;
        };
        switch (sf.type) {
            case FixtureType::DIMMER:
                write_output_dmx(sf.output_index, sf.address, clamp_u8(0.279f * r + 0.547f * g + 0.106f * b));
                break;
            case FixtureType::RGB:
                write_output_dmx(sf.output_index, sf.address + 0, clamp_u8(r / flux.r));
                write_output_dmx(sf.output_index, sf.address + 1, clamp_u8(g / flux.g));
                write_output_dmx(sf.output_index, sf.address + 2, clamp_u8(b / flux.b));
                break;
            case FixtureType::RGBW: {
                float w_perc = (float)std::min({r, g, b});
                write_output_dmx(sf.output_index, sf.address + 0, clamp_u8((r - w_perc) / flux.r));
                write_output_dmx(sf.output_index, sf.address + 1, clamp_u8((g - w_perc) / flux.g));
                write_output_dmx(sf.output_index, sf.address + 2, clamp_u8((b - w_perc) / flux.b));
                write_output_dmx(sf.output_index, sf.address + 3, clamp_u8(w_perc / flux.w));
                break;
            }
        }
    }

    void write_output_dmx(int output_index, int offset, std::uint8_t value)
    {
        if (output_index >= 0 && output_index < (int)outputs_.size() && offset >= 0 && offset < 512)
            outputs_[output_index].buffer[offset] = value;
    }

    // Pull any edge that falls outside the frame back to the nearest in-frame point, warning once
    // if that happens. After this the box is guaranteed to sit within [0, width] x [0, height].
    void clamp_box_to_frame(box& b)
    {
        float left   = std::clamp(b.x, 0.0f, (float)src_width_);
        float top    = std::clamp(b.y, 0.0f, (float)src_height_);
        float right  = std::clamp(b.x + b.width, 0.0f, (float)src_width_);
        float bottom = std::clamp(b.y + b.height, 0.0f, (float)src_height_);

        if (left != b.x || top != b.y || right != b.x + b.width || bottom != b.y + b.height)
            CASPAR_LOG(warning) << L"artnet: fixture box extends outside the frame; "
                                   L"clamping to the nearest point inside it.";

        b.x      = left;
        b.y      = top;
        b.width  = right - left;
        b.height = bottom - top;
    }

    void setup_group_sws(computed_fixture_group& group)
    {
        if (group.sub_fixtures.empty()) {
            CASPAR_LOG(warning) << L"artnet: fixture group has no sub-fixtures; skipping.";
            return;
        }

        // Confine the box to the frame once, so both paths below operate on an in-frame box and
        // there is no axis-aligned vs. rotated difference in how out-of-frame areas are handled.
        clamp_box_to_frame(group.source_box);

        const box& b = group.source_box;
        if (b.width <= 0.0f || b.height <= 0.0f) {
            CASPAR_LOG(warning) << L"artnet: fixture box is empty or outside the channel; skipping.";
            return;
        }

        // Rotated boxes are resampled into a scratch buffer first; axis-aligned boxes take the
        // cheap direct-crop path.
        int sws_src_w = 0, sws_src_h = 0;

        if (group.rotated) {
            auto [w, h] = rotated_dims(group);
            sws_src_w   = w;
            sws_src_h   = h;
        } else {
            group.crop_x = (int)std::round(b.x);
            group.crop_y = (int)std::round(b.y);
            group.crop_w = (int)std::round(b.x + b.width) - group.crop_x;
            group.crop_h = (int)std::round(b.y + b.height) - group.crop_y;

            if (group.crop_w <= 0 || group.crop_h <= 0) {
                CASPAR_LOG(warning) << L"artnet: fixture box is smaller than a pixel; skipping.";
                return;
            }

            sws_src_w = group.crop_w;
            sws_src_h = group.crop_h;
        }

        group.sws.reset(sws_getContext(sws_src_w,
                                       sws_src_h,
                                       AV_PIX_FMT_BGRA,
                                       group.cols,
                                       group.rows,
                                       AV_PIX_FMT_BGRA,
                                       SWS_AREA,
                                       nullptr,
                                       nullptr,
                                       nullptr),
                        [](SwsContext* p) {
                            if (p)
                                sws_freeContext(p);
                        });

        if (!group.sws)
            CASPAR_THROW_EXCEPTION(caspar_exception() << msg_info("artnet: failed to create SwsContext"));

        group.output.assign(group.cols * group.rows * 4, 0);
    }

    void send_dmx_data(const std::uint8_t* data, std::size_t length, int physical_universe, const udp::endpoint& dest)
    {
        std::uint8_t hUni = (physical_universe >> 8) & 0xff;
        std::uint8_t lUni = physical_universe & 0xff;

        std::uint8_t hLen = (length >> 8) & 0xff;
        std::uint8_t lLen = (length & 0xff);

        std::uint8_t header[] = {65, 114, 116, 45, 78, 101, 116, 0, 0, 80, 0, 14, 0, 0, lUni, hUni, hLen, lLen};
        std::uint8_t buffer[18 + 512];

        for (int i = 0; i < 18 + 512; i++) {
            if (i < 18) {
                buffer[i] = header[i];
                continue;
            }

            if (i - 18 < length) {
                buffer[i] = data[i - 18];
                continue;
            }

            buffer[i] = 0;
        }

        boost::system::error_code err;
        socket.send_to(boost::asio::buffer(buffer), dest, 0, err);
        CASPAR_LOG(trace) << "Sent DMX data to Artnet, universe: " << physical_universe;
        if (err)
            CASPAR_THROW_EXCEPTION(io_error() << msg_info(err.message()));
    }
};

std::vector<fixture> get_fixtures_ptree(const boost::property_tree::wptree& ptree)
{
    std::vector<fixture> fixtures;

    using boost::property_tree::wptree;

    for (auto& xml_channel : ptree | witerate_children(L"fixtures") | welement_context_iteration) {
        ptree_verify_element_name(xml_channel, L"fixture");

        fixture f{};

        auto start_opt = xml_channel.second.get_optional<int>(L"start-address");
        if (!start_opt)
            CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"Fixture <start-address> must be specified"));
        if (*start_opt < 1 || *start_opt > 512)
            CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"Fixture <start-address> must be between 1 and 512"));

        f.startAddress = (unsigned short)(*start_opt - 1);

        f.host = xml_channel.second.get(L"host", std::wstring());
        if (f.host.empty())
            CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"Fixture <host> must be specified"));

        boost::system::error_code host_err;
        make_address(u8(f.host), host_err);
        if (host_err)
            CASPAR_THROW_EXCEPTION(user_error()
                                   << msg_info(L"Fixture <host> must be a valid IP address: " + f.host));

        auto port_opt = xml_channel.second.get_optional<int>(L"port");
        if (!port_opt)
            CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"Fixture <port> must be specified"));
        if (*port_opt < 1 || *port_opt > 65535)
            CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"Fixture <port> must be between 1 and 65535"));
        f.port = (unsigned short)*port_opt;

        auto universe_opt = xml_channel.second.get_optional<int>(L"universe");
        if (!universe_opt)
            CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"Fixture <universe> must be specified"));
        if (*universe_opt < 0 || *universe_opt > 32767)
            CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"Fixture <universe> must be between 0 and 32767"));
        f.universe = *universe_opt;

        std::wstring count_str = xml_channel.second.get(L"fixture-count", std::wstring());
        if (count_str.empty())
            CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"Fixture count must be specified"));

        int cols = 0;
        int rows = 1;
        try {
            size_t x_pos = count_str.find(L'x');
            if (x_pos != std::wstring::npos) {
                cols = std::stoi(count_str.substr(0, x_pos));
                rows = std::stoi(count_str.substr(x_pos + 1));
            } else {
                cols = std::stoi(count_str);
            }
        } catch (...) {
            CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"Fixture count must be 'N' or 'WxH'"));
        }

        if (cols < 1 || rows < 1)
            CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"Fixture count must be at least 1 in each dimension"));

        f.fixtureCols = (unsigned short)cols;
        f.fixtureRows = (unsigned short)rows;

        std::wstring type = xml_channel.second.get(L"type", L"");
        if (type.empty())
            CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"Fixture type must be specified"));

        if (boost::iequals(type, L"DIMMER")) {
            f.type = FixtureType::DIMMER;
        } else if (boost::iequals(type, L"RGB")) {
            f.type = FixtureType::RGB;
        } else if (boost::iequals(type, L"RGBW")) {
            f.type = FixtureType::RGBW;
        } else {
            CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"Unknown fixture type"));
        }

        int fixtureChannels = xml_channel.second.get(L"fixture-channels", -1);
        if (fixtureChannels < 0)
            fixtureChannels = f.type;
        if (fixtureChannels < f.type)
            CASPAR_THROW_EXCEPTION(
                user_error() << msg_info(
                    L"Fixture channel count must be at least enough channels for current color mode"));
        if (fixtureChannels > 512)
            CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"Fixture <fixture-channels> must not exceed 512"));

        f.fixtureChannels = (unsigned short)fixtureChannels;

        if (auto flux_child = xml_channel.second.get_child_optional(L"flux")) {
            f.flux.r = flux_child->get(L"r", 1.0f);
            f.flux.g = flux_child->get(L"g", 1.0f);
            f.flux.b = flux_child->get(L"b", 1.0f);
            f.flux.w = flux_child->get(L"w", 1.0f);

            if (f.flux.r <= 0.0f || f.flux.g <= 0.0f || f.flux.b <= 0.0f || f.flux.w <= 0.0f)
                CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"Fixture <flux> values must be positive"));
        }

        f.brightness = xml_channel.second.get(L"brightness", 1.0f);
        if (f.brightness < 0.0f || f.brightness > 1.0f)
            CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"Fixture <brightness> must be between 0.0 and 1.0"));

        if (auto rot_deg = xml_channel.second.get_optional<float>(L"rotation"))
            f.rotation = *rot_deg * PI / 180.0f;

        f.mirror_x = xml_channel.second.get(L"mirror-x", false);
        f.mirror_y = xml_channel.second.get(L"mirror-y", false);

        // Position can be given as center (<x>/<y>), top-left edge (<left>/<top>), or
        // bottom-right edge (<right>/<bottom>). Size is taken from <width>/<height>, or
        // derived from <left>+<right> / <top>+<bottom> when the explicit size is omitted.
        // Center takes priority, then left/top, then right/bottom.
        auto width_opt  = xml_channel.second.get_optional<float>(L"width");
        auto height_opt = xml_channel.second.get_optional<float>(L"height");
        auto cx         = xml_channel.second.get_optional<float>(L"x");
        auto left       = xml_channel.second.get_optional<float>(L"left");
        auto right      = xml_channel.second.get_optional<float>(L"right");
        auto cy         = xml_channel.second.get_optional<float>(L"y");
        auto top        = xml_channel.second.get_optional<float>(L"top");
        auto bottom     = xml_channel.second.get_optional<float>(L"bottom");

        bool derive_width  = !width_opt && !cx && left && right;
        bool derive_height = !height_opt && !cy && top && bottom;

        box b{};
        b.width  = derive_width ? *right - *left : width_opt.value_or(0.0f);
        b.height = derive_height ? *bottom - *top : height_opt.value_or(0.0f);

        if (((cx ? 1 : 0) + (left ? 1 : 0) + (right ? 1 : 0)) > 1 && !derive_width)
            CASPAR_LOG(warning)
                << L"artnet: fixture has conflicting horizontal specifiers. "
                   L"Use one of <x>/<left>/<right> with <width>, or <left>+<right>. "
                   L"Using <x> if present, otherwise <left>, otherwise <right>.";

        if (((cy ? 1 : 0) + (top ? 1 : 0) + (bottom ? 1 : 0)) > 1 && !derive_height)
            CASPAR_LOG(warning)
                << L"artnet: fixture has conflicting vertical specifiers. "
                   L"Use one of <y>/<top>/<bottom> with <height>, or <top>+<bottom>. "
                   L"Using <y> if present, otherwise <top>, otherwise <bottom>.";

        if (cx)
            b.x = *cx - b.width / 2.0f;
        else if (left)
            b.x = *left;
        else if (right)
            b.x = *right - b.width;
        else
            b.x = 0.0f;

        if (cy)
            b.y = *cy - b.height / 2.0f;
        else if (top)
            b.y = *top;
        else if (bottom)
            b.y = *bottom - b.height;
        else
            b.y = 0.0f;

        f.fixtureBox = b;

        fixtures.push_back(f);
    }

    return fixtures;
}

spl::shared_ptr<core::frame_consumer>
create_preconfigured_consumer(const boost::property_tree::wptree&                      ptree,
                              const core::video_format_repository&                     format_repository,
                              const std::vector<spl::shared_ptr<core::video_channel>>& channels,
                              const core::channel_info&                                channel_info)
{
    configuration config;

    if (channel_info.depth != common::bit_depth::bit8)
        CASPAR_THROW_EXCEPTION(caspar_exception() << msg_info("Artnet consumer only supports 8-bit color depth."));

    config.refreshRate = ptree.get(L"refresh-rate", config.refreshRate);

    if (config.refreshRate < 1)
        CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"Refresh rate must be at least 1"));

    config.fixtures = get_fixtures_ptree(ptree);

    return spl::make_shared<artnet_consumer>(config);
}
}} // namespace caspar::artnet
