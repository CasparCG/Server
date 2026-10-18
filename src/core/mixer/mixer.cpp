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

#include "../StdAfx.h"

#include "mixer.h"

#include "../frame/frame.h"

#include "audio/audio_mixer.h"
#include "image/image_mixer.h"

#include <common/diagnostics/graph.h>

#include <core/frame/draw_frame.h>
#include <core/frame/frame_transform.h>
#include <core/frame/pixel_format.h>
#include <core/video_format.h>

#include <queue>
#include <unordered_map>
#include <vector>

namespace caspar { namespace core {

struct mixer::impl
{
    monitor::state                      state_;
    monitor::state                      transforms_;
    int                                 channel_index_;
    spl::shared_ptr<diagnostics::graph> graph_;
    audio_mixer                         audio_mixer_{graph_};
    spl::shared_ptr<image_mixer>        image_mixer_;
    struct rendered_frame
    {
        const_frame    frame;
        monitor::state transforms;
    };
    std::queue<std::future<rendered_frame>> buffer_;

    impl(const impl&)            = delete;
    impl& operator=(const impl&) = delete;

    impl(int channel_index, spl::shared_ptr<diagnostics::graph> graph, spl::shared_ptr<image_mixer> image_mixer)
        : channel_index_(channel_index)
        , graph_(std::move(graph))
        , image_mixer_(std::move(image_mixer))
    {
    }

    /// Advances the render queue and exposes transforms only for the frame successfully returned.
    const_frame operator()(std::vector<draw_frame>  frames,
                           const video_format_desc& format_desc,
                           int                      nb_samples,
                           monitor::state           transforms)
    {
        image_mixer_->update_aspect_ratio(static_cast<double>(format_desc.square_width) /
                                          static_cast<double>(format_desc.square_height));

        for (auto& frame : frames) {
            frame.accept(audio_mixer_);
            frame.transform().image_transform.layer_depth = 1;
            frame.accept(*image_mixer_);
        }

        auto result = image_mixer_->render(format_desc);
        auto audio  = audio_mixer_(format_desc, nb_samples);

        state_["audio"] = audio_mixer_.state();

        auto depth = image_mixer_->depth();

        buffer_.push(std::async(
            std::launch::deferred,
            [result = std::move(result),
             audio  = std::move(audio),
             graph  = graph_,
             depth,
             format_desc,
             transforms = std::move(transforms),
             tag        = this]() mutable {
                auto desc = pixel_format_desc(pixel_format::bgra);
                desc.planes.push_back(pixel_format_desc::plane(format_desc.width, format_desc.height, 4, depth));
                std::vector<array<const uint8_t>> image_data;
                auto                              tuple = std::move(result.get());
                image_data.emplace_back(std::move(std::get<0>(tuple)));
                return rendered_frame{
                    const_frame(tag, std::move(image_data), std::move(audio), desc, std::move(std::get<1>(tuple))),
                    std::move(transforms)};
            }));

        if (buffer_.size() <= format_desc.field_count) {
            return const_frame{};
        }

        auto frame = std::move(buffer_.front().get());
        buffer_.pop();
        transforms_ = std::move(frame.transforms);
        return frame.frame;
    }

    void set_master_volume(float volume) { audio_mixer_.set_master_volume(volume); }

    float get_master_volume() { return audio_mixer_.get_master_volume(); }
};

mixer::mixer(int channel_index, spl::shared_ptr<diagnostics::graph> graph, spl::shared_ptr<image_mixer> image_mixer)
    : impl_(new impl(channel_index, std::move(graph), std::move(image_mixer)))
{
}
void        mixer::set_master_volume(float volume) { impl_->set_master_volume(volume); }
float       mixer::get_master_volume() { return impl_->get_master_volume(); }
/// Carries transform metadata through the same buffering path as its rendered image.
const_frame mixer::operator()(std::vector<draw_frame>  frames,
                              const video_format_desc& format_desc,
                              int                      nb_samples,
                              monitor::state           transforms)
{
    return (*impl_)(std::move(frames), format_desc, nb_samples, std::move(transforms));
}
mutable_frame mixer::create_frame(const void* tag, const pixel_format_desc& desc)
{
    return impl_->image_mixer_->create_frame(tag, desc);
}
core::monitor::state mixer::state() const { return impl_->state_; }
/// Reports completed render state rather than transforms still waiting in the mixer buffer.
core::monitor::state mixer::transforms() const { return impl_->transforms_; }

common::bit_depth mixer::depth() const { return impl_->image_mixer_->depth(); }
}} // namespace caspar::core
