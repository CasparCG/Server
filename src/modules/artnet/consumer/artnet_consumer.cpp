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

#include <boost/algorithm/string.hpp>
#include <boost/asio.hpp>
#include <boost/property_tree/ptree.hpp>

#include <thread>
#include <utility>
#include <vector>

using namespace boost::asio;
using namespace boost::asio::ip;

namespace caspar { namespace artnet {

struct configuration
{
    int            universe = 0;
    std::wstring   host     = L"127.0.0.1";
    unsigned short port     = 6454;

    int refreshRate = 10;

    std::vector<fixture> fixtures;
};

struct artnet_consumer : public core::frame_consumer
{
    const configuration           config;
    std::vector<computed_fixture> computed_fixtures;

  public:
    // frame_consumer

    explicit artnet_consumer(configuration config)
        : config(std::move(config))
        , io_context_()
        , socket(io_context_)
    {
        socket.open(udp::v4());

        std::string host_ = u8(this->config.host);
        remote_endpoint   = boost::asio::ip::udp::endpoint(boost::asio::ip::make_address(host_), this->config.port);

        compute_fixtures();
    }

    void initialize(const core::video_format_desc& /*format_desc*/,
                    const core::channel_info& channel_info,
                    int                       port_index) override
    {
        thread_ = std::thread([this] {
            long long time      = 1000 / config.refreshRate;
            auto      last_send = std::chrono::system_clock::now();

            while (!abort_request_) {
                try {
                    auto                          now             = std::chrono::system_clock::now();
                    std::chrono::duration<double> elapsed_seconds = now - last_send;
                    long long                     elapsed_ms =
                        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed_seconds).count();

                    long long sleep_time = time - elapsed_ms * 1000;
                    if (sleep_time > 0)
                        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_time));

                    last_send = now;

                    frame_mutex_.lock();
                    auto frame = last_frame_;

                    frame_mutex_.unlock();
                    if (!frame)
                        continue; // No frame available

                    uint8_t dmx_data[512];
                    memset(dmx_data, 0, 512);

                    for (auto computed_fixture : computed_fixtures) {
                        auto     color = average_color(frame, computed_fixture.rectangle);
                        uint8_t* ptr   = dmx_data + computed_fixture.address;

                        switch (computed_fixture.type) {
                            case FixtureType::DIMMER:
                                ptr[0] = (uint8_t)(0.279 * color.r + 0.547 * color.g + 0.106 * color.b);
                                break;
                            case FixtureType::RGB:
                                ptr[0] = color.r;
                                ptr[1] = color.g;
                                ptr[2] = color.b;
                                break;
                            case FixtureType::RGBW:
                                uint8_t w = std::min(std::min(color.r, color.g), color.b);
                                ptr[0]    = color.r - w;
                                ptr[1]    = color.g - w;
                                ptr[2]    = color.b - w;
                                ptr[3]    = w;
                                break;
                        }
                    }

                    send_dmx_data(dmx_data, 512);
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

    std::wstring print() const override
    {
        auto id_str = consumer_id().empty() ? L"" : L"|id=" + consumer_id();
        return L"artnet[" + id_str + L"]";
    }

    std::wstring name() const override { return L"artnet"; }

    int index() const override { return 1337; }

    core::monitor::state state() const override
    {
        core::monitor::state state;
        state["artnet/computed-fixtures"] = computed_fixtures.size();
        state["artnet/fixtures"]          = config.fixtures.size();
        state["artnet/universe"]          = config.universe;
        state["artnet/host"]              = config.host;
        state["artnet/port"]              = config.port;
        state["artnet/refresh-rate"]      = config.refreshRate;
        if (!consumer_id().empty())
            state["artnet/consumer_id"] = consumer_id();

        return state;
    }

  private:
    core::const_frame last_frame_;
    std::mutex        frame_mutex_;

    std::thread       thread_;
    std::atomic<bool> abort_request_{false};

    io_context    io_context_;
    udp::socket   socket;
    udp::endpoint remote_endpoint;

    void compute_fixtures()
    {
        computed_fixtures.clear();
        for (auto fixture : config.fixtures) {
            for (unsigned short i = 0; i < fixture.fixtureCount; i++) {
                computed_fixture computed_fixture{};
                computed_fixture.type    = fixture.type;
                computed_fixture.address = fixture.startAddress + i * fixture.fixtureChannels;

                computed_fixture.rectangle = compute_rect(fixture.fixtureBox, i, fixture.fixtureCount);
                computed_fixtures.push_back(computed_fixture);
            }
        }
    }

    void send_dmx_data(const std::uint8_t* data, std::size_t length)
    {
        int universe = this->config.universe;

        std::uint8_t hUni = (universe >> 8) & 0xff;
        std::uint8_t lUni = universe & 0xff;

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
        socket.send_to(boost::asio::buffer(buffer), remote_endpoint, 0, err);
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

        int startAddress = xml_channel.second.get(L"start-address", 0);
        if (startAddress < 1)
            CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"Fixture start address must be specified"));

        f.startAddress = (unsigned short)startAddress - 1;

        int fixtureCount = xml_channel.second.get(L"fixture-count", -1);
        if (fixtureCount < 1)
            CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"Fixture count must be specified"));

        f.fixtureCount = (unsigned short)fixtureCount;

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

        f.fixtureChannels = (unsigned short)fixtureChannels;

        box b{};

        auto x = xml_channel.second.get(L"x", 0.0f);
        auto y = xml_channel.second.get(L"y", 0.0f);

        b.x = x;
        b.y = y;

        auto width  = xml_channel.second.get(L"width", 0.0f);
        auto height = xml_channel.second.get(L"height", 0.0f);

        b.width  = width;
        b.height = height;

        auto rotation = xml_channel.second.get(L"rotation", 0.0f);

        b.rotation   = rotation;
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

    config.universe    = ptree.get(L"universe", config.universe);
    config.host        = ptree.get(L"host", config.host);
    config.port        = ptree.get(L"port", config.port);
    config.refreshRate = ptree.get(L"refresh-rate", config.refreshRate);

    if (config.refreshRate < 1)
        CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"Refresh rate must be at least 1"));

    config.fixtures = get_fixtures_ptree(ptree);

    return spl::make_shared<artnet_consumer>(config);
}
}} // namespace caspar::artnet
