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
#include "server.h"
#include "included_modules.h"

#include <accelerator/accelerator.h>

#include <common/env.h>
#include <common/except.h>
#include <common/memory.h>
#include <common/ptree.h>
#include <common/utf.h>

#include <core/consumer/output.h>
#include <core/diagnostics/call_context.h>
#include <core/diagnostics/osd_graph.h>
#include <core/mixer/image/image_mixer.h>
#include <core/mixer/mixer.h>
#include <core/producer/cg_proxy.h>
#include <core/producer/color/color_producer.h>
#include <core/producer/frame_producer.h>
#include <core/producer/stage.h>
#include <core/video_channel.h>
#include <core/video_format.h>

#include <modules/image/consumer/image_consumer.h>

#include <protocol/amcp/AMCPCommandsImpl.h>
#include <protocol/amcp/AMCPProtocolStrategy.h>
#include <protocol/amcp/amcp_command_repository.h>
#include <protocol/cii/CIIProtocolStrategy.h>
#include <protocol/clk/CLKProtocolStrategy.h>
#include <protocol/osc/client.h>
#include <protocol/util/AsyncEventServer.h>
#include <protocol/util/strategy_adapters.h>
#include <protocol/util/tokenize.h>

#include <boost/algorithm/string.hpp>
#include <boost/asio.hpp>
#include <boost/format.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>

#include <future>
#include <thread>

namespace caspar {

using namespace core;
using namespace protocol;

std::shared_ptr<boost::asio::io_service> create_running_io_service()
{
    auto service = std::make_shared<boost::asio::io_service>();
    // To keep the io_service::run() running although no pending async
    // operations are posted.
    auto work      = std::make_shared<boost::asio::io_service::work>(*service);
    auto weak_work = std::weak_ptr<boost::asio::io_service::work>(work);
    auto thread    = std::make_shared<std::thread>([service, weak_work] {
        while (auto strong = weak_work.lock()) {
            try {
                service->run();
            } catch (...) {
                CASPAR_LOG_CURRENT_EXCEPTION();
            }
        }

        CASPAR_LOG(info) << "[asio] Global io_service uninitialized.";
    });

    return std::shared_ptr<boost::asio::io_service>(service.get(), [service, work, thread](void*) mutable {
        CASPAR_LOG(info) << "[asio] Shutting down global io_service.";
        work.reset();
        service->stop();
        if (thread->get_id() != std::this_thread::get_id())
            thread->join();
        else
            thread->detach();
    });
}

struct server::impl : boost::noncopyable
{
    std::shared_ptr<boost::asio::io_service>               io_service_ = create_running_io_service();
    accelerator::accelerator                               accelerator_;
    std::shared_ptr<amcp::amcp_command_repository>         amcp_command_repo_;
    std::shared_ptr<amcp::amcp_command_repository_wrapper> amcp_command_repo_wrapper_;
    std::shared_ptr<amcp::command_context_factory>         amcp_context_factory_;
    std::shared_ptr<amcp::AMCPCommandScheduler>            amcp_command_scheduler_;
    std::vector<spl::shared_ptr<IO::AsyncEventServer>>     async_servers_;
    std::shared_ptr<IO::AsyncEventServer>                  primary_amcp_server_;
    std::shared_ptr<osc::client>                           osc_client_ = std::make_shared<osc::client>(io_service_);
    std::vector<std::shared_ptr<void>>                     predefined_osc_subscriptions_;
    std::vector<spl::shared_ptr<video_channel>>            channels_;
    spl::shared_ptr<core::cg_producer_registry>            cg_registry_;
    spl::shared_ptr<core::frame_producer_registry>         producer_registry_;
    spl::shared_ptr<core::frame_consumer_registry>         consumer_registry_;
    std::function<void(bool)>                              shutdown_server_now_;

    explicit impl(std::function<void(bool)> shutdown_server_now)
        : accelerator_(env::properties().get(L"configuration.accelerator", L"auto"))
        , producer_registry_(spl::make_shared<core::frame_producer_registry>())
        , consumer_registry_(spl::make_shared<core::frame_consumer_registry>())
        , shutdown_server_now_(shutdown_server_now)
    {
        caspar::core::diagnostics::osd::register_sink();

        module_dependencies dependencies(cg_registry_, producer_registry_, consumer_registry_);

        initialize_modules(dependencies);
        core::init_cg_proxy_as_producer(dependencies);
    }

    void start()
    {
        auto xml_channels = setup_channels(env::properties());
        CASPAR_LOG(info) << L"Initialized channels.";

        setup_amcp_command_repo();
        CASPAR_LOG(info) << L"Initialized command repository.";

        setup_channel_producers(xml_channels);
        CASPAR_LOG(info) << L"Initialized channel predefined producers.";

        setup_controllers(env::properties());
        CASPAR_LOG(info) << L"Initialized controllers.";

        setup_osc(env::properties());
        CASPAR_LOG(info) << L"Initialized osc.";
    }

    ~impl()
    {
        std::weak_ptr<boost::asio::io_service> weak_io_service = io_service_;
        io_service_.reset();
        osc_client_.reset();
        amcp_command_repo_wrapper_.reset();
        amcp_command_repo_.reset();
        amcp_context_factory_.reset();
        amcp_command_scheduler_.reset();
        primary_amcp_server_.reset();
        async_servers_.clear();
        destroy_producers_synchronously();
        destroy_consumers_synchronously();
        channels_.clear();

        while (weak_io_service.lock())
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

        uninitialize_modules();
        core::diagnostics::osd::shutdown();
    }

    std::vector<boost::property_tree::wptree> setup_channels(const boost::property_tree::wptree& pt)
    {
        using boost::property_tree::wptree;

        std::vector<wptree> xml_channels;

        for (auto& xml_channel : pt | witerate_children(L"configuration.channels") | welement_context_iteration) {
            xml_channels.push_back(xml_channel.second);
            ptree_verify_element_name(xml_channel, L"channel");

            auto format_desc_str = xml_channel.second.get(L"video-mode", L"PAL");
            auto format_desc     = video_format_desc(format_desc_str);
            if (format_desc.format == video_format::invalid)
                CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"Invalid video-mode: " + format_desc_str));

            auto weak_client = std::weak_ptr<osc::client>(osc_client_);
            auto channel_id  = static_cast<int>(channels_.size() + 1);
            auto channel =
                spl::make_shared<video_channel>(channel_id,
                                                format_desc,
                                                accelerator_.create_image_mixer(channel_id),
                                                [channel_id, weak_client](core::monitor::state channel_state) {
                                                    monitor::state state;
                                                    state[""]["channel"][channel_id] = channel_state;
                                                    auto client                      = weak_client.lock();
                                                    if (client) {
                                                        client->send(std::move(state));
                                                    }
                                                });

            channels_.push_back(channel);
        }

        for (auto& channel : channels_) {
            core::diagnostics::scoped_call_context save;
            core::diagnostics::call_context::for_thread().video_channel = channel->index();

            if (xml_channels.at(channel->index() - 1).get_child_optional(L"consumers")) {
                for (auto& xml_consumer : xml_channels.at(channel->index() - 1) | witerate_children(L"consumers") |
                                              welement_context_iteration) {
                    auto name = xml_consumer.first;

                    try {
                        if (name != L"<xmlcomment>")
                            channel->output().add(
                                consumer_registry_->create_consumer(name, xml_consumer.second, channels_));
                    } catch (...) {
                        CASPAR_LOG_CURRENT_EXCEPTION();
                    }
                }
            }
        }

        return xml_channels;
    }

    void setup_osc(const boost::property_tree::wptree& pt)
    {
        using boost::property_tree::wptree;
        using namespace boost::asio::ip;

        auto default_port                 = pt.get<unsigned short>(L"configuration.osc.default-port", 6250);
        auto disable_send_to_amcp_clients = pt.get(L"configuration.osc.disable-send-to-amcp-clients", false);
        auto predefined_clients           = pt.get_child_optional(L"configuration.osc.predefined-clients");

        if (predefined_clients) {
            for (auto& predefined_client :
                 pt | witerate_children(L"configuration.osc.predefined-clients") | welement_context_iteration) {
                ptree_verify_element_name(predefined_client, L"predefined-client");

                const auto address = ptree_get<std::wstring>(predefined_client.second, L"address");
                const auto port    = ptree_get<unsigned short>(predefined_client.second, L"port");

                boost::system::error_code ec;
                auto                      ipaddr = address_v4::from_string(u8(address), ec);
                if (!ec)
                    predefined_osc_subscriptions_.push_back(
                        osc_client_->get_subscription_token(udp::endpoint(ipaddr, port)));
                else
                    CASPAR_LOG(warning) << "Invalid OSC client. Must be valid ipv4 address: " << address;
            }
        }

        if (!disable_send_to_amcp_clients && primary_amcp_server_)
            primary_amcp_server_->add_client_lifecycle_object_factory(
                [=](const std::string& ipv4_address) -> std::pair<std::wstring, std::shared_ptr<void>> {
                    using namespace boost::asio::ip;

                    return std::make_pair(std::wstring(L"osc_subscribe"),
                                          osc_client_->get_subscription_token(
                                              udp::endpoint(address_v4::from_string(ipv4_address), default_port)));
                });
    }

    static std::vector<amcp::channel_context>
    build_channel_contexts(const std::vector<spl::shared_ptr<core::video_channel>>& channels)
    {
        std::vector<amcp::channel_context> res;

        int index = 0;
        for (const auto& channel : channels) {
            const std::wstring lifecycle_key = L"lock" + std::to_wstring(index);
            res.emplace_back(channel, channel->stage(), lifecycle_key);
            ++index;
        }

        return res;
    }

    void setup_channel_producers(const std::vector<boost::property_tree::wptree>& xml_channels)
    {
        auto console_client = spl::make_shared<IO::ConsoleClientInfo>();

        for (auto& channel : channels_) {
            core::diagnostics::scoped_call_context save;
            core::diagnostics::call_context::for_thread().video_channel = channel->index();

            auto xml_channel = xml_channels.at(channel->index() - 1);

            if (xml_channel.get_child_optional(L"producers")) {
                for (auto& xml_producer : xml_channel | witerate_children(L"producers") | welement_context_iteration) {
                    ptree_verify_element_name(xml_producer, L"producer");

                    const std::wstring command = xml_producer.second.get_value(L"");
                    const auto         attrs   = xml_producer.second.get_child(L"<xmlattr>");
                    const int          id      = attrs.get(L"id", -1);

                    try {
                        std::list<std::wstring> tokens{L"PLAY",
                                                       (boost::wformat(L"%i-%i") % channel->index() % id).str()};
                        IO::tokenize(command, tokens);
                        auto cmd = amcp_command_repo_->parse_command(console_client, tokens, L"");

                        std::wstring res = cmd->Execute(amcp_command_repo_->channels()).get();
                        console_client->send(std::move(res), false);
                    } catch (const user_error&) {
                        CASPAR_LOG(error) << "Failed to parse command: " << command;
                    } catch (...) {
                        CASPAR_LOG_CURRENT_EXCEPTION();
                    }
                }
            }

            const std::wstring source = xml_channel.get(L"timecode", L"free");
            if (boost::iequals(source, "clock")) {
                channel->timecode()->set_system_time(); // TODO - offset/timezone
            } else if (boost::iequals(source, "layer")) {
                const int layer = xml_channel.get(L"timecode_layer", 0);

                // Run it on the stage to ensure the producer creation has completed fully
                channel->stage()->execute([channel, layer]() {
                    std::shared_ptr<frame_producer> producer = channel->stage()->foreground(layer).get();

                    // Unwrap to remove the transition
                    if (producer->following_producer(true) != frame_producer::empty())
                        producer = producer->following_producer(true);

                    if (!channel->timecode()->set_weak_source(producer)) {
                        CASPAR_LOG(error)
                            << L"timecode[" << channel->index() << L"] failed to set timecode from layer " << layer;
                    }
                });
            } else {
                channel->timecode()->clear_source();
            }
        }
    }

    void setup_amcp_command_repo()
    {
        amcp_command_scheduler_ = std::make_shared<amcp::AMCPCommandScheduler>();

        amcp_command_repo_ = std::make_shared<amcp::amcp_command_repository>(build_channel_contexts(channels_));

        auto ctx = std::make_shared<amcp::amcp_command_static_context>(
            cg_registry_,
            producer_registry_,
            consumer_registry_,
            spl::make_shared_ptr(amcp_command_scheduler_),
            amcp_command_repo_,
            shutdown_server_now_,
            u8(caspar::env::properties().get(L"configuration.amcp.media-server.host", L"127.0.0.1")),
            u8(caspar::env::properties().get(L"configuration.amcp.media-server.port", L"8000")));

        amcp_context_factory_ = std::make_shared<amcp::command_context_factory>(ctx);

        amcp_command_repo_wrapper_ =
            std::make_shared<amcp::amcp_command_repository_wrapper>(amcp_command_repo_, amcp_context_factory_);

        amcp::register_commands(amcp_command_repo_wrapper_);
    }

    void setup_controllers(const boost::property_tree::wptree& pt)
    {
        using boost::property_tree::wptree;
        for (auto& xml_controller : pt | witerate_children(L"configuration.controllers") | welement_context_iteration) {
            auto name     = xml_controller.first;
            auto protocol = ptree_get<std::wstring>(xml_controller.second, L"protocol");

            if (name == L"tcp") {
                auto port              = ptree_get<unsigned int>(xml_controller.second, L"port");
                auto asyncbootstrapper = spl::make_shared<IO::AsyncEventServer>(
                    io_service_,
                    create_protocol(protocol, L"TCP Port " + boost::lexical_cast<std::wstring>(port)),
                    static_cast<short>(port));
                async_servers_.push_back(asyncbootstrapper);

                if (!primary_amcp_server_ && boost::iequals(protocol, L"AMCP"))
                    primary_amcp_server_ = asyncbootstrapper;
            } else
                CASPAR_LOG(warning) << "Invalid controller: " << name;
        }
    }

    IO::protocol_strategy_factory<char>::ptr create_protocol(const std::wstring& name,
                                                             const std::wstring& port_description) const
    {
        using namespace IO;

        if (boost::iequals(name, L"AMCP"))
            return amcp::create_char_amcp_strategy_factory(port_description,
                                                           spl::make_shared_ptr(amcp_command_repo_),
                                                           spl::make_shared_ptr(amcp_command_scheduler_));
        else if (boost::iequals(name, L"CII"))
            return wrap_legacy_protocol(
                "\r\n", spl::make_shared<cii::CIIProtocolStrategy>(channels_, cg_registry_, producer_registry_));
        else if (boost::iequals(name, L"CLOCK"))
            return spl::make_shared<to_unicode_adapter_factory>(
                "ISO-8859-1",
                spl::make_shared<CLK::clk_protocol_strategy_factory>(channels_, cg_registry_, producer_registry_));

        CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"Invalid protocol: " + name));
    }
};

server::server(std::function<void(bool)> shutdown_server_now)
    : impl_(new impl(shutdown_server_now))
{
}
void                                                     server::start() { impl_->start(); }
spl::shared_ptr<protocol::amcp::amcp_command_repository> server::get_amcp_command_repository() const
{
    return spl::make_shared_ptr(impl_->amcp_command_repo_);
}
spl::shared_ptr<amcp::AMCPCommandScheduler> server::get_amcp_command_scheduler() const
{
    return spl::make_shared_ptr(impl_->amcp_command_scheduler_);
}

} // namespace caspar
