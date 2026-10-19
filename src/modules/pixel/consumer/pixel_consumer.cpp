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
 * Author: Dimitry Ishenko, dimitry (dot) ishenko (at) (gee) mail (dot) com
 */

#include "artdmx_sink.h"
#include "color_grader.h"
#include "pixel_consumer.h"
#include "types.h"

#include "common/except.h"
#include "common/executor.h"
#include "common/future.h"
#include "common/log.h"

#include "core/consumer/channel_info.h"
#include "core/consumer/frame_consumer.h"
#include "core/frame/frame.h"

#include <boost/property_tree/ptree.hpp>

#include <cctype>
#include <chrono>
#include <exception>
#include <locale>
#include <ranges>
#include <span>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace caspar::pixel {

using namespace std::chrono_literals;
using steady_clock = std::chrono::steady_clock;

struct pixel_consumer : public core::frame_consumer
{
    artdmx_sink sink_;
    color_grader grader_;
    pixel_type type_;

    executor executor_{L"pixel_consumer"};

    int channel_index_ = -1;
    core::video_format_desc format_desc_;

    bool failed_ = false;
    steady_clock::time_point since_;

public:
    pixel_consumer(artdmx_sink sink, color_grader grader, pixel_type type) :
        sink_{std::move(sink)},
        grader_{std::move(grader)},
        type_{type}
    { }

    void initialize(const core::video_format_desc& format_desc,
                    const core::channel_info&      channel_info,
                    int                            port_index) override
    {
        channel_index_ = channel_info.index;
        format_desc_   = format_desc;
    }

    std::future<bool> send(core::video_field field, core::const_frame frame) override
    {
        executor_.begin_invoke([this, frame = std::move(frame)]
        {
            auto raw = frame.image_data(0);
            std::span pix{ (const pixel*)raw.data(), raw.size() / sizeof(pixel) };

            auto grade = [this](auto fn) {
                using namespace std::views;
                return transform([this, fn](pixel p) { return (grader_.*fn)(p); }) | join;
            };

            try {
                switch (type_)
                {
                    case luma: sink_.push(pix | grade(&color_grader::to_luma)); break;
                    case rgb : sink_.push(pix | grade(&color_grader::to_rgb )); break;
                    case rgbw: sink_.push(pix | grade(&color_grader::to_rgbw)); break;
                    case rgbx: sink_.push(pix | grade(&color_grader::to_rgbx)); break;
                }

                if (failed_ && steady_clock::now() > since_ + 3s) {
                    CASPAR_LOG(info) << print() << ": Connection restored";
                    failed_ = false;
                }
            } catch (const std::exception& e) {
                if (!failed_) CASPAR_LOG(error) << print() << ": " << e.what();
                failed_ = true;
                since_ = steady_clock::now();
            }
        });

        return make_ready_future(true);
    }

    std::wstring print() const override
    {
        return name() + L'[' + std::to_wstring(channel_index_) + L'|' + format_desc_.name + L']';
    }

    std::wstring name() const override { return L"pixel"; }

    int index() const override { return 1000; }

    core::monitor::state state() const override
    {
        static const core::monitor::state empty;
        return empty;
    }
};

spl::shared_ptr<core::frame_consumer>
create_preconfigured_consumer(const boost::property_tree::wptree&                      ptree,
                              const core::video_format_repository&                     format_repository,
                              const std::vector<spl::shared_ptr<core::video_channel>>& channels,
                              const core::channel_info&                                channel_info)
{
    if (channel_info.depth != common::bit_depth::bit8)
        CASPAR_THROW_EXCEPTION(caspar_exception() << msg_info("Unsupported color depth."));

    auto tolower = []<typename C>(std::basic_string<C> s) {
        for (auto& c : s) c = std::tolower(c, std::locale{});
        return s;
    };

    auto protocol = tolower(ptree.get(L"protocol", L""));
    if (protocol != L"artnet")
        CASPAR_THROW_EXCEPTION(user_error() << msg_info("Unsupported or unspecified protocol."));

    auto host = ptree.get(L"host", L"127.0.0.1");
    auto port = ptree.get<std::uint16_t>(L"port", 6454);

    auto universe = ptree.get<std::uint16_t>(L"universe", 0);
    if (universe >= 32768)
        CASPAR_THROW_EXCEPTION(user_error() << msg_info("Invalid universe number."));

    auto address = ptree.get<std::uint16_t>(L"start-address", 1);
    if (address < 1 || address > 512)
        CASPAR_THROW_EXCEPTION(user_error() << msg_info("Invalid start address."));

    // start address is 1-based for the user (DMX spec), but 0-based for artdmx_sink
    artdmx_sink sink{ u8(host), port, universe, --address };

    static const std::unordered_map<std::wstring, pixel_type> types{
        { L"luma", luma },
        { L"rgb" , rgb  },
        { L"rgbw", rgbw },
        { L"rgbx", rgbx },
    };
    auto ti = types.find(tolower(ptree.get(L"type", L"")));
    if (ti == types.end())
        CASPAR_THROW_EXCEPTION(user_error() << msg_info("Unsupported or unspecified pixel type."));

    auto ptree_get = [&](auto... name) {
        return std::make_tuple(ptree.get(name, 1.0f)...);
    };
    auto [r, g, b, w, γ] = ptree_get(L"coef.r", L"coef.g", L"coef.b", L"coef.w", L"gamma");

    if (r <= 0 || g <= 0 || b <= 0 || w <= 0)
        CASPAR_THROW_EXCEPTION(user_error() << msg_info("Invalid luminance coefficient(s)."));

    if (γ < 0.1 || γ > 10)
        CASPAR_THROW_EXCEPTION(user_error() << msg_info("Invalid gamma value."));

    color_grader grader{coef{r, g, b, w}, γ};

    return spl::make_shared<pixel_consumer>(std::move(sink), std::move(grader), ti->second);
}

} // namespace caspar::pixel
