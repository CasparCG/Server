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

#include "../../StdAfx.h"

#include "separated_producer.h"

#include <common/scope_exit.h>

#include <core/frame/draw_frame.h>
#include <core/monitor/monitor.h>
#include <core/producer/frame_producer.h>

#include <future>
#include <mutex>

namespace caspar { namespace core {

class separated_producer : public frame_producer
{
    monitor::state state_;

    spl::shared_ptr<frame_producer> fill_producer_;
    spl::shared_ptr<frame_producer> key_producer_;
    draw_frame                      fill_;
    draw_frame                      key_;

  public:
    explicit separated_producer(const spl::shared_ptr<frame_producer>& fill, const spl::shared_ptr<frame_producer>& key)
        : fill_producer_(fill)
        , key_producer_(key)
    {
        CASPAR_LOG(debug) << print() << L" Initialized";
    }

    // frame_producer

    draw_frame last_frame() override
    {
        return draw_frame::mask(fill_producer_->last_frame(), key_producer_->last_frame());
    }
    draw_frame first_frame() override
    {
        return draw_frame::mask(fill_producer_->first_frame(), key_producer_->first_frame());
    }

    draw_frame receive_impl(int nb_samples) override
    {
        CASPAR_SCOPE_EXIT
        {
            state_          = fill_producer_->state();
            state_["keyer"] = key_producer_->state();
        };

        if (!fill_) {
            fill_ = fill_producer_->receive(nb_samples);
        }

        if (!key_) {
            key_ = key_producer_->receive(nb_samples);
        }

        if (!fill_ || !key_) {
            return core::draw_frame{};
        }

        auto frame = draw_frame::mask(fill_, key_);

        fill_ = draw_frame{};
        key_  = draw_frame{};

        return frame;
    }

    uint32_t frame_number() const override { return fill_producer_->frame_number(); }

    uint32_t nb_frames() const override { return std::min(fill_producer_->nb_frames(), key_producer_->nb_frames()); }

    std::wstring print() const override
    {
        return L"separated[fill:" + fill_producer_->print() + L"|key[" + key_producer_->print() + L"]]";
    }

    std::future<std::wstring> call(const std::vector<std::wstring>& params) override
    {
        key_producer_->call(params);
        return fill_producer_->call(params);
    }

    std::wstring name() const override { return L"separated"; }

    core::monitor::state state() const override { return state_; }

    const frame_timecode& timecode() override { return fill_producer_->timecode(); }
    bool                  has_timecode() override { return fill_producer_->has_timecode(); }
    bool                  provides_timecode() override { return fill_producer_->provides_timecode(); }
};

spl::shared_ptr<frame_producer> create_separated_producer(const spl::shared_ptr<frame_producer>& fill,
                                                          const spl::shared_ptr<frame_producer>& key)
{
    return spl::make_shared<separated_producer>(fill, key);
}

}} // namespace caspar::core
