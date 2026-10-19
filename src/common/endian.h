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
 * Author: Helge Norberg, helge.norberg@svt.se
 * Author: Robert Nagy, ronag89@gmail.com
 * Author: Dimitry Ishenko, dimitry (dot) ishenko (at) (gee) mail (dot) com
 */

#pragma once

#include <bit> // std::endian
#include <concepts>
#include <cstddef> // std::size_t
#include <type_traits>

#if defined(_MSC_VER)
#include <stdlib.h> // _byteswap_...
#endif

template <typename T, std::size_t N>
concept integral_of_size = std::integral<T> && (sizeof(T) == N);

namespace caspar {

template <integral_of_size<1> V>
constexpr auto swap_byte_order(V val) { return val; }

template <integral_of_size<2> V>
constexpr auto swap_byte_order(V val)
{
    auto uval = static_cast<std::make_unsigned_t<V>>(val);

#if defined(_MSC_VER)
    auto result = _byteswap_ushort(uval);
#elif defined(__GNUC__) || defined(__clang__)
    auto result = __builtin_bswap16(uval);
#else
    auto result = ((uval & 0x00ffu) << 8) |
                  ((uval & 0xff00u) >> 8);
#endif

    return static_cast<V>(result);
}

template <integral_of_size<4> V>
constexpr auto swap_byte_order(V val)
{
    auto uval = static_cast<std::make_unsigned_t<V>>(val);

#if defined(_MSC_VER)
    auto result = _byteswap_ulong(uval);
#elif defined(__GNUC__) || defined(__clang__)
    auto result = __builtin_bswap32(uval);
#else
    auto result = ((uval & 0x000000ffu) << 24) |
                  ((uval & 0x0000ff00u) <<  8) |
                  ((uval & 0x00ff0000u) >>  8) |
                  ((uval & 0xff000000u) >> 24);
#endif

    return static_cast<V>(result);
}

template <integral_of_size<8> V>
constexpr auto swap_byte_order(V val)
{
    auto uval = static_cast<std::make_unsigned_t<V>>(val);

#if defined(_MSC_VER)
    auto result = _byteswap_uint64(uval);
#elif defined(__GNUC__) || defined(__clang__)
    auto result = __builtin_bswap64(uval);
#else
    auto result = ((uval & 0x00000000000000ffull) << 56) |
                  ((uval & 0x000000000000ff00ull) << 40) |
                  ((uval & 0x0000000000ff0000ull) << 24) |
                  ((uval & 0x00000000ff000000ull) <<  8) |
                  ((uval & 0x000000ff00000000ull) >>  8) |
                  ((uval & 0x0000ff0000000000ull) >> 24) |
                  ((uval & 0x00ff000000000000ull) >> 40) |
                  ((uval & 0xff00000000000000ull) >> 56);
#endif

    return static_cast<V>(result);
}

constexpr auto little_endian(std::integral auto val)
{
    if constexpr (std::endian::native == std::endian::big)
        return swap_byte_order(val);
    else return val;
}

constexpr auto big_endian(std::integral auto val)
{
    if constexpr (std::endian::native == std::endian::little)
        return swap_byte_order(val);
    else return val;
}

} // namespace caspar
