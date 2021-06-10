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

#include "stage.h"

#include "layer.h"

#include "../frame/draw_frame.h"

#include <common/diagnostics/graph.h>
#include <common/executor.h>
#include <common/future.h>

#include <core/frame/frame_transform.h>
#include <core/producer/route/route_producer.h>

#include <boost/range/adaptors.hpp>

#include <functional>
#include <future>
#include <map>
#include <vector>

namespace caspar { namespace core {

struct stage::impl : public std::enable_shared_from_this<impl>
{
    int                                 channel_index_;
    spl::shared_ptr<diagnostics::graph> graph_;
    monitor::state                      state_;
    std::map<int, layer>                layers_;
    std::map<int, tweened_transform>    tweens_;
    std::set<int>                       routeSources;

    executor executor_{L"stage " + std::to_wstring(channel_index_)};

  private:
    void orderSourceLayers(std::vector<std::pair<int, bool>>&        layerVec,
                           const std::map<int, std::pair<int, int>>& routed_layers,
                           int                                       l,
                           int                                       depth)
    {
        if (0 == depth)
            routeSources.clear();

        if (std::find_if(layerVec.begin(), layerVec.end(), [l](std::pair<int, bool> p) { return p.first == l; }) != layerVec.end()) {
            return;
        }

        auto routeIt = routed_layers.find(l);
        if (routed_layers.end() == routeIt) {
            layerVec.push_back(std::make_pair(l, true));
            return;
        }

        std::pair<int, int> routeSrc(routeIt->second);
        if (channel_index_ != routeSrc.first) {
            layerVec.push_back(std::make_pair(l, true));
            return;
        }

        // check for circular route setup - skip recursion if found
        routeSources.emplace(routeSrc.second);
        bool layerOK = true;
        if (routeSources.find(l) == routeSources.end()) {
            orderSourceLayers(layerVec, routed_layers, routeSrc.second, ++depth);
        } else {
            layerOK = false;
        }

        if (std::find_if(layerVec.begin(), layerVec.end(), [l](std::pair<int, bool> p) { return p.first == l; }) == layerVec.end()) {
            layerVec.push_back(std::make_pair(l, layerOK));
        }
    }

  public:
    impl(int channel_index, spl::shared_ptr<diagnostics::graph> graph)
        : channel_index_(channel_index)
        , graph_(std::move(graph))
    {
    }

    std::vector<draw_frame> operator()(const video_format_desc&                     format_desc,
                                       int                                          nb_samples,
                                       std::vector<int>&                            fetch_background,
                                       std::function<void(int, const layer_frame&)> routesCb)
    {
        return executor_.invoke([=] {
            std::map<int, layer_frame> frames;
            std::vector<draw_frame>    stage_frames;

            try {
                for (auto& t : tweens_)
                    t.second.tick(1);

                // build a map of layers that are sourced from route producers
                std::map<int, std::pair<int, int>> routed_layers;
                for (auto& p : layers_) {
                    auto producer = std::move(p.second.foreground());
                    if (0 == producer->name().compare(L"route")) {
                        try {
                            auto rc       = spl::dynamic_pointer_cast<core::route_control>(producer);
                            auto srcChan  = rc->get_source_channel();
                            auto srcLayer = rc->get_source_layer();
                            routed_layers.emplace(p.first, std::make_pair(srcChan, srcLayer));
                            rc->set_cross_channel(channel_index_ != srcChan);
                        } catch (std::bad_cast) {
                            CASPAR_LOG(error) << "Failed to cast route producer";
                        }
                    }
                }

                // sort layer order so that sources get pulled before routes
                std::vector<std::pair<int, bool>> layerVec;
                for (auto& p : layers_)
                    orderSourceLayers(layerVec, routed_layers, p.first, 0);

                for (auto& l : layerVec) {
                    auto  p     = layers_.find(l.first);
                    auto& layer = p->second;
                    auto& tween = tweens_[p->first];

                    layer_frame res    = {};
                    res.foreground     = draw_frame::push(l.second ? layer.receive(format_desc, nb_samples) : draw_frame(), tween.fetch());
                    res.has_background = layer.has_background();
                    if (std::find(fetch_background.begin(), fetch_background.end(), p->first) !=
                        fetch_background.end()) {
                        res.background = layer.receive_background(format_desc, nb_samples);
                    }
                    frames[p->first] = res;

                    // push received foreground frame to any configured route producer
                    routesCb(p->first, res);
                }

                for (auto& p : frames) {
                    stage_frames.push_back(p.second.foreground);
                }

                // push stage_frames to support any channel routes that have been set
                layer_frame chan_lf = {};
                chan_lf.foreground  = draw_frame(stage_frames);
                routesCb(-1, chan_lf);

                monitor::state state;
                for (auto& p : layers_) {
                    state["layer"][p.first] = p.second.state();
                }
                state_ = std::move(state);
            } catch (...) {
                layers_.clear();
                CASPAR_LOG_CURRENT_EXCEPTION();
            }

            return stage_frames;
        });
    }

    layer& get_layer(int index)
    {
        auto it = layers_.find(index);
        if (it == std::end(layers_)) {
            it = layers_.emplace(index, layer()).first;
        }
        return it->second;
    }

    std::future<void>
    apply_transforms(const std::vector<std::tuple<int, stage::transform_func_t, unsigned int, tweener>>& transforms)
    {
        return executor_.begin_invoke([=] {
            for (auto& transform : transforms) {
                auto& tween = tweens_[std::get<0>(transform)];
                auto  src   = tween.fetch();
                auto  dst   = std::get<1>(transform)(tween.dest());
                tweens_[std::get<0>(transform)] =
                    tweened_transform(src, dst, std::get<2>(transform), std::get<3>(transform));
            }
        });
    }

    std::future<void> apply_transform(int                            index,
                                      const stage::transform_func_t& transform,
                                      unsigned int                   mix_duration,
                                      const tweener&                 tween)
    {
        return executor_.begin_invoke([=] {
            auto src       = tweens_[index].fetch();
            auto dst       = transform(src);
            tweens_[index] = tweened_transform(src, dst, mix_duration, tween);
        });
    }

    std::future<void> clear_transforms(int index)
    {
        return executor_.begin_invoke([=] { tweens_.erase(index); });
    }

    std::future<void> clear_transforms()
    {
        return executor_.begin_invoke([=] { tweens_.clear(); });
    }

    std::future<frame_transform> get_current_transform(int index)
    {
        return executor_.begin_invoke([=] { return tweens_[index].fetch(); });
    }

    std::future<void> load(int index, const spl::shared_ptr<frame_producer>& producer, bool preview, bool auto_play)
    {
        return executor_.begin_invoke([=] { get_layer(index).load(producer, preview, auto_play); });
    }
    
	std::future<void> preview(int index)
    {
        return executor_.begin_invoke([=] { get_layer(index).preview(); });
    }

    std::future<void> pause(int index)
    {
        return executor_.begin_invoke([=] { get_layer(index).pause(); });
    }

    std::future<void> resume(int index)
    {
        return executor_.begin_invoke([=] { get_layer(index).resume(); });
    }

    std::future<void> play(int index)
    {
        return executor_.begin_invoke([=] { get_layer(index).play(); });
    }

    std::future<void> stop(int index)
    {
        return executor_.begin_invoke([=] { get_layer(index).stop(); });
    }

    std::future<void> clear(int index)
    {
        return executor_.begin_invoke([=] { layers_.erase(index); });
    }

    std::future<void> clear()
    {
        return executor_.begin_invoke([=] { layers_.clear(); });
    }

    std::future<void> swap_layers(stage& other, bool swap_transforms)
    {
        auto other_impl = other.impl_;

        if (other_impl.get() == this) {
            return make_ready_future();
        }

        auto func = [=] {
            auto layers       = layers_ | boost::adaptors::map_values;
            auto other_layers = other_impl->layers_ | boost::adaptors::map_values;

            std::swap(layers_, other_impl->layers_);

            if (swap_transforms)
                std::swap(tweens_, other_impl->tweens_);
        };

        return invoke_both(other, func);
    }

    std::future<void> swap_layer(int index, int other_index, bool swap_transforms)
    {
        return executor_.begin_invoke([=] {
            std::swap(get_layer(index), get_layer(other_index));

            if (swap_transforms)
                std::swap(tweens_[index], tweens_[other_index]);
        });
    }

    std::future<void> swap_layer(int index, int other_index, stage& other, bool swap_transforms)
    {
        auto other_impl = other.impl_;

        if (other_impl.get() == this)
            return swap_layer(index, other_index, swap_transforms);
        auto func = [=] {
            auto& my_layer    = get_layer(index);
            auto& other_layer = other_impl->get_layer(other_index);

            std::swap(my_layer, other_layer);

            if (swap_transforms) {
                auto& my_tween    = tweens_[index];
                auto& other_tween = other_impl->tweens_[other_index];
                std::swap(my_tween, other_tween);
            }
        };

        return invoke_both(other, func);
    }

    std::future<void> invoke_both(stage& other, std::function<void()> func)
    {
        auto other_impl = other.impl_;

        if (other_impl->channel_index_ < channel_index_) {
            return other_impl->executor_.begin_invoke([=] { executor_.invoke(func); });
        }

        return executor_.begin_invoke([=] { other_impl->executor_.invoke(func); });
    }

    std::future<std::shared_ptr<frame_producer>> foreground(int index)
    {
        return executor_.begin_invoke(
            [=]() -> std::shared_ptr<frame_producer> { return get_layer(index).foreground(); });
    }

    std::future<std::shared_ptr<frame_producer>> background(int index)
    {
        return executor_.begin_invoke(
            [=]() -> std::shared_ptr<frame_producer> { return get_layer(index).background(); });
    }

    std::future<std::wstring> call(int index, const std::vector<std::wstring>& params)
    {
        return flatten(executor_.begin_invoke([=] { return get_layer(index).foreground()->call(params).share(); }));
    }
};

stage::stage(int channel_index, spl::shared_ptr<diagnostics::graph> graph)
    : impl_(new impl(channel_index, std::move(graph)))
{
}
std::future<std::wstring> stage::call(int index, const std::vector<std::wstring>& params)
{
    return impl_->call(index, params);
}
std::future<void> stage::apply_transforms(const std::vector<stage::transform_tuple_t>& transforms)
{
    return impl_->apply_transforms(transforms);
}
std::future<void> stage::apply_transform(int                                                                index,
                                         const std::function<core::frame_transform(core::frame_transform)>& transform,
                                         unsigned int   mix_duration,
                                         const tweener& tween)
{
    return impl_->apply_transform(index, transform, mix_duration, tween);
}
std::future<void>            stage::clear_transforms(int index) { return impl_->clear_transforms(index); }
std::future<void>            stage::clear_transforms() { return impl_->clear_transforms(); }
std::future<frame_transform> stage::get_current_transform(int index) { return impl_->get_current_transform(index); }
std::future<void> stage::load(int index, const spl::shared_ptr<frame_producer>& producer, bool preview, bool auto_play)
{
    return impl_->load(index, producer, preview, auto_play);
}
std::future<void> stage::preview(int index) { return impl_->preview(index); }
std::future<void> stage::pause(int index) { return impl_->pause(index); }
std::future<void> stage::resume(int index) { return impl_->resume(index); }
std::future<void> stage::play(int index) { return impl_->play(index); }
std::future<void> stage::stop(int index) { return impl_->stop(index); }
std::future<void> stage::clear(int index) { return impl_->clear(index); }
std::future<void> stage::clear() { return impl_->clear(); }
std::future<void> stage::swap_layers(stage& other, bool swap_transforms)
{
    return impl_->swap_layers(other, swap_transforms);
}
std::future<void> stage::swap_layer(int index, int other_index, bool swap_transforms)
{
    return impl_->swap_layer(index, other_index, swap_transforms);
}
std::future<void> stage::swap_layer(int index, int other_index, stage& other, bool swap_transforms)
{
    return impl_->swap_layer(index, other_index, other, swap_transforms);
}
std::future<std::shared_ptr<frame_producer>> stage::foreground(int index) { return impl_->foreground(index); }
std::future<std::shared_ptr<frame_producer>> stage::background(int index) { return impl_->background(index); }
std::vector<draw_frame> stage::operator()(const video_format_desc&                     format_desc,
                                          int                                          nb_samples,
                                          std::vector<int>&                            fetch_background,
                                          std::function<void(int, const layer_frame&)> routesCb)
{
    return (*impl_)(format_desc, nb_samples, fetch_background, routesCb);
}
core::monitor::state stage::state() const { return impl_->state_; }
}} // namespace caspar::core
