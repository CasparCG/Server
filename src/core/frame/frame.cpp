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
#include "frame.h"

#include "geometry.h"
#include "pixel_format.h"

#include <common/array.h>
#include <common/except.h>

#include <cstdint>
#include <functional>
#include <vector>

namespace caspar { namespace core {

struct mutable_frame::impl
{
    std::vector<array<std::uint8_t>> image_data_;
    array<std::int32_t>              audio_data_;
    const core::pixel_format_desc    desc_;
    const void*                      tag_;
    frame_geometry                   geometry_ = frame_geometry::get_default();
    mutable_frame::commit_t          commit_;

    impl(const impl&) = delete;
    impl& operator=(const impl&) = delete;

    impl(const void*                      tag,
         std::vector<array<std::uint8_t>> image_data,
         array<std::int32_t>              audio_data,
         const core::pixel_format_desc&   desc,
         commit_t                         commit)
        : image_data_(std::move(image_data))
        , audio_data_(std::move(audio_data))
        , desc_(desc)
        , tag_(tag)
        , commit_(std::move(commit))
    {
    }
};

mutable_frame::mutable_frame(const void*                      tag,
                             std::vector<array<std::uint8_t>> image_data,
                             array<int32_t>                   audio_data,
                             const core::pixel_format_desc&   desc,
                             commit_t                         commit)
    : impl_(new impl(tag, std::move(image_data), std::move(audio_data), desc, std::move(commit)))
{
}
mutable_frame::mutable_frame(mutable_frame&& other)
    : impl_(std::move(other.impl_))
{
}
mutable_frame::~mutable_frame() {}
mutable_frame& mutable_frame::operator=(mutable_frame&& other)
{
    impl_ = std::move(other.impl_);
    return *this;
}
void                           mutable_frame::swap(mutable_frame& other) { impl_.swap(other.impl_); }
const core::pixel_format_desc& mutable_frame::pixel_format_desc() const { return impl_->desc_; }
const array<std::uint8_t>& mutable_frame::image_data(std::size_t index) const { return impl_->image_data_.at(index); }
const array<std::int32_t>& mutable_frame::audio_data() const { return impl_->audio_data_; }
array<std::uint8_t>&       mutable_frame::image_data(std::size_t index) { return impl_->image_data_.at(index); }
array<std::int32_t>&       mutable_frame::audio_data() { return impl_->audio_data_; }
std::size_t                mutable_frame::width() const { return impl_->desc_.planes.at(0).width; }
std::size_t                mutable_frame::height() const { return impl_->desc_.planes.at(0).height; }
const void*                mutable_frame::stream_tag() const { return impl_->tag_; }
const frame_geometry&      mutable_frame::geometry() const { return impl_->geometry_; }
frame_geometry&            mutable_frame::geometry() { return impl_->geometry_; }

struct const_frame::impl
{
    std::vector<array<const std::uint8_t>> image_data_;
    array<const std::int32_t>              audio_data_;
    core::pixel_format_desc                desc_     = pixel_format::invalid;
    const void*                            tag_;
    frame_geometry                         geometry_ = frame_geometry::get_default();
    boost::any                             opaque_;

    impl(const void*                            tag,
         std::vector<array<const std::uint8_t>> image_data,
         array<const std::int32_t>              audio_data,
         const core::pixel_format_desc&         desc)
        : tag_(tag)
        , image_data_(std::move(image_data))
        , audio_data_(std::move(audio_data))
        , desc_(desc)
    {
        if (desc_.planes.size() != image_data_.size()) {
            CASPAR_THROW_EXCEPTION(invalid_argument());
        }
    }

    impl(const void*                        tag,
         std::vector<array<std::uint8_t>>&& image_data,
         array<const std::int32_t>          audio_data,
         const core::pixel_format_desc&     desc)
        : tag_(tag)
        , image_data_(std::make_move_iterator(image_data.begin()), std::make_move_iterator(image_data.end()))
        , audio_data_(std::move(audio_data))
        , desc_(desc)
    {
        if (desc_.planes.size() != image_data_.size()) {
            CASPAR_THROW_EXCEPTION(invalid_argument());
        }
    }

    impl(mutable_frame&& other)
        : tag_(other.stream_tag())
        , image_data_(std::make_move_iterator(other.impl_->image_data_.begin()),
                      std::make_move_iterator(other.impl_->image_data_.end()))
        , audio_data_(std::move(other.impl_->audio_data_))
        , desc_(std::move(other.impl_->desc_))
        , geometry_(std::move(other.impl_->geometry_))
    {
        if (desc_.planes.size() != image_data_.size() && !other.impl_->commit_) {
            CASPAR_THROW_EXCEPTION(invalid_argument());
        }
        if (other.impl_->commit_) {
            opaque_ = other.impl_->commit_(image_data_);
        }
    }

    const array<const std::uint8_t>& image_data(std::size_t index) const { return image_data_.at(index); }

    std::size_t width() const { return desc_.planes.at(0).width; }

    std::size_t height() const { return desc_.planes.at(0).height; }

    std::size_t size() const { return desc_.planes.at(0).size; }
};

const_frame::const_frame() {}
const_frame::const_frame(std::vector<array<const std::uint8_t>> image_data,
                         array<const std::int32_t>              audio_data,
                         const core::pixel_format_desc&         desc)
    : impl_(new impl(nullptr, std::move(image_data), std::move(audio_data), desc))
{
}
const_frame::const_frame(mutable_frame&& other)
    : impl_(new impl(std::move(other)))
{
}
const_frame::const_frame(const const_frame& other)
    : impl_(other.impl_)
{
}
const_frame::~const_frame() {}
const_frame& const_frame::operator=(const const_frame& other)
{
    impl_ = other.impl_;
    return *this;
}
bool const_frame::operator==(const const_frame& other) const { return impl_ == other.impl_; }
bool const_frame::operator!=(const const_frame& other) const { return !(*this == other); }
bool const_frame::operator<(const const_frame& other) const { return impl_ < other.impl_; }
bool const_frame::               operator>(const const_frame& other) const { return impl_ > other.impl_; }
const pixel_format_desc&         const_frame::pixel_format_desc() const { return impl_->desc_; }
const array<const std::uint8_t>& const_frame::image_data(std::size_t index) const { return impl_->image_data(index); }
const array<const std::int32_t>& const_frame::audio_data() const { return impl_->audio_data_; }
std::size_t                      const_frame::width() const { return impl_->width(); }
std::size_t                      const_frame::height() const { return impl_->height(); }
std::size_t                      const_frame::size() const { return impl_->size(); }
const void*                      const_frame::stream_tag() const { return impl_->tag_; }
const_frame                      const_frame::with_tag(const void* new_tag) const {
    const_frame copy(*this);
    copy.impl_->tag_ = new_tag;
    return copy;
}
const frame_geometry&            const_frame::geometry() const { return impl_->geometry_; }
const boost::any&                const_frame::opaque() const { return impl_->opaque_; }
const_frame::operator bool() const { return impl_ != nullptr && impl_->desc_.format != core::pixel_format::invalid; }
}} // namespace caspar::core
