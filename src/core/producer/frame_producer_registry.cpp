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
 * Author: Julian Waller, julian@superfly.tv
 */

#include "../StdAfx.h"

#include "cg_proxy.h"
#include "frame_producer.h"
#include "frame_producer_registry.h"

#include "color/color_producer.h"
#include "route/route_producer.h"
#include "separated/separated_producer.h"

#include <boost/algorithm/string/predicate.hpp>

namespace caspar { namespace core {

frame_producer_registry::frame_producer_registry() {}

void frame_producer_registry::register_producer_factory(std::wstring name, const producer_factory_t& factory)
{
    producer_factories_.push_back(factory);
}

std::shared_ptr<executor>& producer_destroyer()
{
    static auto destroyer = [] {
        auto result = std::make_shared<executor>(L"Producer destroyer");
        result->set_capacity(std::numeric_limits<unsigned int>::max());
        return result;
    }();

    return destroyer;
}

std::atomic<bool>& destroy_producers_in_separate_thread()
{
    static std::atomic<bool> state;

    return state;
}

void destroy_producers_synchronously()
{
    destroy_producers_in_separate_thread() = false;
    // Join destroyer, executing rest of producers in queue synchronously.
    producer_destroyer().reset();
}

class destroy_producer_proxy : public frame_producer
{
    std::shared_ptr<frame_producer> producer_;

  public:
    destroy_producer_proxy(spl::shared_ptr<frame_producer>&& producer)
        : producer_(std::move(producer))
    {
        destroy_producers_in_separate_thread() = true;
    }

    virtual ~destroy_producer_proxy()
    {
        if (producer_ == core::frame_producer::empty() || !destroy_producers_in_separate_thread())
            return;

        auto destroyer = producer_destroyer();

        if (!destroyer)
            return;

        // Backpressure for the asynchronous destroyer queue.
        //
        // The queue used to be guarded by CASPAR_VERIFY(size < 8), but throwing from a
        // destructor while many layers are being cleared at once is unsafe and can itself
        // abort the process. Simply dropping the guard would let the backlog (and the memory
        // of every not-yet-destroyed producer) grow without bound if destruction cannot keep
        // up. Instead, once the backlog passes a threshold, destroy this producer
        // synchronously on the current thread. That bounds the queue and provides real
        // backpressure without ever throwing.
        constexpr std::size_t destroyer_backlog_limit = 8;

        if (destroyer->size() >= destroyer_backlog_limit) {
            CASPAR_LOG(warning) << L"Producer destroyer backlog (" << destroyer->size()
                                << L") reached limit; destroying synchronously to apply backpressure.";
            try {
                producer_.reset();
            } catch (...) {
                CASPAR_LOG_CURRENT_EXCEPTION();
            }
            return;
        }

        auto producer = new spl::shared_ptr<frame_producer>(std::move(producer_));

        destroyer->begin_invoke([=] {
            std::unique_ptr<spl::shared_ptr<frame_producer>> pointer_guard(producer);
            auto                                             str = (*producer)->print();
            try {
                if (producer->use_count() != 1)
                    CASPAR_LOG(debug) << str << L" Not destroyed on asynchronous destruction thread: "
                                      << producer->use_count();
                else
                    CASPAR_LOG(debug) << str << L" Destroying on asynchronous destruction thread.";
            } catch (...) {
            }

            try {
                pointer_guard.reset();
                CASPAR_LOG(info) << str << L" Destroyed.";
            } catch (...) {
                CASPAR_LOG_CURRENT_EXCEPTION();
            }
        });
    }

    draw_frame receive_impl(const core::video_field field, int nb_samples) override
    {
        return producer_->receive(field, nb_samples);
    }
    std::wstring              print() const override { return producer_->print(); }
    std::wstring              name() const override { return producer_->name(); }
    std::future<std::wstring> call(const std::vector<std::wstring>& params) override { return producer_->call(params); }
    void                      leading_producer(const spl::shared_ptr<frame_producer>& producer) override
    {
        return producer_->leading_producer(producer);
    }
    uint32_t             frame_number() const override { return producer_->frame_number(); }
    uint32_t             nb_frames() const override { return producer_->nb_frames(); }
    draw_frame           last_frame(const core::video_field field) override { return producer_->last_frame(field); }
    draw_frame           first_frame(const core::video_field field) override { return producer_->first_frame(field); }
    core::monitor::state state() const override { return producer_->state(); }
    bool                 is_ready() override { return producer_->is_ready(); }
};

spl::shared_ptr<core::frame_producer> do_create_producer(const frame_producer_dependencies&     dependencies,
                                                         const std::vector<std::wstring>&       params,
                                                         const std::vector<producer_factory_t>& factories,
                                                         bool                                   throw_on_fail = false)
{
    if (params.empty()) {
        CASPAR_THROW_EXCEPTION(invalid_argument() << msg_info("params cannot be empty"));
    }

    auto producer = create_color_producer(dependencies.frame_factory, params);
    if (producer != frame_producer::empty()) {
        return producer;
    }

    producer = create_route_producer(dependencies, params);
    if (producer != frame_producer::empty()) {
        return producer;
    }

    if (std::any_of(factories.begin(), factories.end(), [&](const producer_factory_t& factory) -> bool {
            try {
                producer = factory(dependencies, params);
            } catch (user_error&) {
                throw;
            } catch (...) {
                if (throw_on_fail)
                    throw;
                else
                    CASPAR_LOG_CURRENT_EXCEPTION();
            }
            return producer != frame_producer::empty();
        })) {
        return producer;
    }
    return frame_producer::empty();
}

spl::shared_ptr<core::frame_producer>
frame_producer_registry::create_producer(const frame_producer_dependencies& dependencies,
                                         const std::vector<std::wstring>&   params) const
{
    auto& producer_factories = producer_factories_;
    auto  producer           = do_create_producer(dependencies, params, producer_factories);
    auto  key_producer       = frame_producer::empty();

    if (!params.empty() && !boost::contains(params.at(0), L"://")) {
        try // to find a key file.
        {
            auto params_copy = params;
            params_copy[0] += L"_A";
            key_producer = do_create_producer(dependencies, params_copy, producer_factories);
            if (key_producer == frame_producer::empty()) {
                params_copy[0] += L"LPHA";
                key_producer = do_create_producer(dependencies, params_copy, producer_factories);
            }
        } catch (...) {
        }
    }

    if (producer != frame_producer::empty() && key_producer != frame_producer::empty())
        return create_separated_producer(producer, key_producer);

    if (producer == frame_producer::empty()) {
        std::wstring str;
        for (auto& param : params)
            str += param + L" ";
        CASPAR_THROW_EXCEPTION(file_not_found() << msg_info("No match found for supplied commands. Check syntax.")
                                                << arg_value_info(u8(str)));
    }

    // Skip destroy_producer_proxy for route_producer, as it needs to be able to perform this cast
    // This isn't a nice approach, but it keeps it simple and ensures every other producer has the
    // destroy_producer_proxy wrapping
    if (spl::instance_of<core::route_control>(producer)) {
        return std::move(producer);
    }

    return spl::make_shared<destroy_producer_proxy>(std::move(producer));
}

spl::shared_ptr<core::frame_producer>
frame_producer_registry::create_producer(const frame_producer_dependencies& dependencies,
                                         const std::wstring&                params) const
{
    std::wstringstream        iss(params);
    std::vector<std::wstring> tokens;
    using iterator = std::istream_iterator<std::wstring, wchar_t, std::char_traits<wchar_t>>;
    std::copy(iterator(iss), iterator(), std::back_inserter(tokens));
    return create_producer(dependencies, tokens);
}
}} // namespace caspar::core
