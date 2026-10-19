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

#include "frame_consumer.h"
#include "frame_consumer_registry.h"

#include <core/frame/frame.h>

#include <boost/circular_buffer.hpp>
#include <boost/property_tree/ptree.hpp>

#include <future>
#include <map>
#include <vector>

namespace caspar { namespace core {

std::atomic<bool>& destroy_consumers_in_separate_thread()
{
    static std::atomic<bool> state;

    return state;
}

void destroy_consumers_synchronously() { destroy_consumers_in_separate_thread() = false; }

class destroy_consumer_proxy : public frame_consumer
{
    std::shared_ptr<frame_consumer> consumer_;

  public:
    destroy_consumer_proxy(spl::shared_ptr<frame_consumer>&& consumer)
        : consumer_(std::move(consumer))
    {
        destroy_consumers_in_separate_thread() = true;
    }

    ~destroy_consumer_proxy()
    {
        static std::atomic<int> counter;
        static std::once_flag   counter_init_once;
        std::call_once(counter_init_once, [] { counter = 0; });

        if (!destroy_consumers_in_separate_thread())
            return;

        ++counter;
        CASPAR_VERIFY(counter < 8);

        auto consumer = new std::shared_ptr<frame_consumer>(std::move(consumer_));
        std::thread([=] {
            std::unique_ptr<std::shared_ptr<frame_consumer>> pointer_guard(consumer);
            auto                                             str = (*consumer)->print();

            try {
                if (consumer->use_count() != 1)
                    CASPAR_LOG(debug) << str << L" Not destroyed on asynchronous destruction thread: "
                                      << consumer->use_count();
                else
                    CASPAR_LOG(debug) << str << L" Destroying on asynchronous destruction thread.";
            } catch (...) {
            }

            pointer_guard.reset();
            counter--;
        }).detach();
    }

    std::future<bool> send(const core::video_field field, const_frame frame) override
    {
        return consumer_->send(field, std::move(frame));
    }
    void
    initialize(const video_format_desc& format_desc, const core::channel_info& channel_info, int port_index) override
    {
        return consumer_->initialize(format_desc, channel_info, port_index);
    }
    std::future<bool>    call(const std::vector<std::wstring>& params) override { return consumer_->call(params); }
    std::wstring         print() const override { return consumer_->print(); }
    std::wstring         name() const override { return consumer_->name(); }
    bool                 has_synchronization_clock() const override { return consumer_->has_synchronization_clock(); }
    int                  index() const override { return consumer_->index(); }
    core::monitor::state state() const override { return consumer_->state(); }
    void set_consumer_id(const std::wstring& id) override
    {
        frame_consumer::set_consumer_id(id);
        consumer_->set_consumer_id(id);
    }
};

class print_consumer_proxy : public frame_consumer
{
    std::shared_ptr<frame_consumer> consumer_;

  public:
    print_consumer_proxy(spl::shared_ptr<frame_consumer>&& consumer)
        : consumer_(std::move(consumer))
    {
    }

    ~print_consumer_proxy()
    {
        auto str = consumer_->print();
        CASPAR_LOG(debug) << str << L" Uninitializing.";
        consumer_.reset();
        CASPAR_LOG(info) << str << L" Uninitialized.";
    }

    std::future<bool> send(const core::video_field field, const_frame frame) override
    {
        return consumer_->send(field, std::move(frame));
    }
    void
    initialize(const video_format_desc& format_desc, const core::channel_info& channel_info, int port_index) override
    {
        consumer_->initialize(format_desc, channel_info, port_index);
        CASPAR_LOG(info) << consumer_->print() << L" Initialized.";
    }
    std::future<bool>    call(const std::vector<std::wstring>& params) override { return consumer_->call(params); }
    std::wstring         print() const override { return consumer_->print(); }
    std::wstring         name() const override { return consumer_->name(); }
    bool                 has_synchronization_clock() const override { return consumer_->has_synchronization_clock(); }
    int                  index() const override { return consumer_->index(); }
    core::monitor::state state() const override { return consumer_->state(); }
    void set_consumer_id(const std::wstring& id) override
    {
        frame_consumer::set_consumer_id(id);
        consumer_->set_consumer_id(id);
    }
};

frame_consumer_registry::frame_consumer_registry() {}

void frame_consumer_registry::register_consumer_factory(const std::wstring& name, const consumer_factory_t& factory)
{
    consumer_factories_.push_back(factory);
}

void frame_consumer_registry::register_preconfigured_consumer_factory(const std::wstring& element_name,
                                                                      const preconfigured_consumer_factory_t& factory)
{
    preconfigured_consumer_factories_.insert(std::make_pair(element_name, factory));
}

spl::shared_ptr<core::frame_consumer>
frame_consumer_registry::create_consumer(const std::vector<std::wstring>&                         params,
                                         const core::video_format_repository&                     format_repository,
                                         const std::vector<spl::shared_ptr<core::video_channel>>& channels,
                                         const core::channel_info&                                channel_info) const
{
    if (params.empty())
        CASPAR_THROW_EXCEPTION(invalid_argument() << msg_info("params cannot be empty"));

    auto  consumer           = frame_consumer::empty();
    auto& consumer_factories = consumer_factories_;
    if (!std::any_of(
            consumer_factories.begin(), consumer_factories.end(), [&](const consumer_factory_t& factory) -> bool {
                try {
                    consumer = factory(params, format_repository, channels, channel_info);
                } catch (...) {
                    CASPAR_LOG_CURRENT_EXCEPTION();
                }
                return consumer != frame_consumer::empty();
            })) {
        CASPAR_THROW_EXCEPTION(file_not_found() << msg_info("No match found for supplied commands. Check syntax."));
    }

    return spl::make_shared<destroy_consumer_proxy>(spl::make_shared<print_consumer_proxy>(std::move(consumer)));
}

spl::shared_ptr<frame_consumer>
frame_consumer_registry::create_consumer(const std::wstring&                                      element_name,
                                         const boost::property_tree::wptree&                      element,
                                         const core::video_format_repository&                     format_repository,
                                         const std::vector<spl::shared_ptr<core::video_channel>>& channels,
                                         const core::channel_info&                                channel_info) const
{
    auto& preconfigured_consumer_factories = preconfigured_consumer_factories_;
    auto  found                            = preconfigured_consumer_factories.find(element_name);

    if (found == preconfigured_consumer_factories.end())
        CASPAR_THROW_EXCEPTION(user_error()
                               << msg_info(L"No consumer factory registered for element name " + element_name));

    return spl::make_shared<destroy_consumer_proxy>(
        spl::make_shared<print_consumer_proxy>(found->second(element, format_repository, channels, channel_info)));
}

}} // namespace caspar::core
