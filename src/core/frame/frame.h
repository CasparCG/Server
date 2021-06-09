#pragma once

#include <common/array.h>

#include <boost/any.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace caspar { namespace core {

class mutable_frame final
{
    friend class const_frame;

  public:
    using commit_t = std::function<boost::any(std::vector<array<const std::uint8_t>>)>;

    explicit mutable_frame(const void*                      tag,
                           std::vector<array<std::uint8_t>> image_data,
                           array<std::int32_t>              audio_data,
                           const struct pixel_format_desc&  desc,
                           commit_t                         commit = nullptr);
    mutable_frame(const mutable_frame&) = delete;
    mutable_frame(mutable_frame&& other);

    ~mutable_frame();

    mutable_frame& operator=(const mutable_frame&) = delete;
    mutable_frame& operator                        =(mutable_frame&& other);

    void swap(mutable_frame& other);

    const struct pixel_format_desc& pixel_format_desc() const;

    array<std::uint8_t>&       image_data(std::size_t index);
    const array<std::uint8_t>& image_data(std::size_t index) const;

    array<std::int32_t>&       audio_data();
    const array<std::int32_t>& audio_data() const;

    std::size_t width() const;

    std::size_t height() const;

    const void* stream_tag() const;

    class frame_geometry&       geometry();
    const class frame_geometry& geometry() const;

  private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

class const_frame final
{
  public:
    const_frame();
    explicit const_frame(std::vector<array<const std::uint8_t>> image_data,
                         array<const std::int32_t>              audio_data,
                         const struct pixel_format_desc&        desc);
    const_frame(const const_frame& other);
    const_frame(mutable_frame&& other);

    ~const_frame();

    const_frame& operator=(const const_frame& other);

    const struct pixel_format_desc& pixel_format_desc() const;

    const array<const std::uint8_t>& image_data(std::size_t index) const;

    const array<const std::int32_t>& audio_data() const;

    std::size_t width() const;

    std::size_t height() const;

    std::size_t size() const;

    const void* stream_tag() const;
    const_frame with_tag(const void* new_tag) const;

    const boost::any& opaque() const;

    const class frame_geometry& geometry() const;

    bool operator==(const const_frame& other) const;
    bool operator!=(const const_frame& other) const;
    bool operator<(const const_frame& other) const;
    bool operator>(const const_frame& other) const;

    explicit operator bool() const;

  private:
    struct impl;
    std::shared_ptr<impl> impl_;
};

}} // namespace caspar::core
