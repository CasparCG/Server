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

#include "common/endian.h"
#include "types.h"

#include <array>
#include <boost/asio.hpp>
#include <cassert>
#include <cstdint>
#include <memory>
#include <ranges>
#include <string_view>
#include <vector>

#pragma warning(push)
#pragma warning(disable : 4267)

namespace caspar::pixel {

namespace asio = boost::asio;

class artdmx_sink
{
#pragma pack(push,1)
    struct head
    {
        const char id[8] = "Art-Net";
        const std::uint16_t opcode  = little_endian(0x5000); // ArtDMX
        const std::uint16_t protver = big_endian<std::uint16_t>(14); // v14
        const std::uint8_t  seq = 0; // ignore
        const std::uint8_t  phy = 0; // ignore

        std::uint16_t universe; // little endian
        std::uint16_t length;   // big endian
    };
#pragma pack(pop)

    std::unique_ptr<asio::io_context> io_;
    asio::ip::udp::socket socket_;

    std::uint16_t universe_, address_;

public:
    artdmx_sink(std::string_view host, std::uint16_t port, std::uint16_t universe, std::uint16_t address) :
        io_{std::make_unique<asio::io_context>()},
        socket_{*io_},
        universe_{universe}, address_{address}
    {
        assert(universe < 32768);
        assert(address < 512);

        using namespace boost::asio::ip;
        socket_.open(udp::v4());
        socket_.connect(udp::endpoint{ make_address(host), port });
    }

    template<std::ranges::input_range R>
        requires std::same_as<std::ranges::range_value_t<R>, level>
    void push(R&& range)
    {
        auto verse = universe_;

        std::vector<level> payload;
        payload.reserve(512);
        payload.resize(address_, 0);

        auto send_payload = [&]{
            head head{
                .universe = little_endian(verse),
                .length = big_endian<std::uint16_t>(payload.size()),
            };
            socket_.send(std::array{
                asio::buffer(&head, sizeof(head)),
                asio::buffer(payload)
            }, 0);
        };

        for (auto&& val : range) {
            payload.push_back(val);

            if (payload.size() == 512) {
                send_payload();

                payload.clear();
                if (++verse == 32768) break;
            }
        }

        if (payload.size()) send_payload();
    }
};

} // namespace caspar::pixel
