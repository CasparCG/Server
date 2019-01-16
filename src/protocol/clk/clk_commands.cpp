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
 * Author: Helge Norberg, helge.norberg@svt.se
 */

#include "../StdAfx.h"

#include <future>
#include <sstream>
#include <stdexcept>

#include <boost/lexical_cast.hpp>

#include <common/log.h>
#include <common/memory.h>

#include <core/producer/cg_proxy.h>
#include <core/producer/stage.h>
#include <core/video_channel.h>

#include "clk_commands.h"

namespace caspar { namespace protocol { namespace CLK {

class command_context
{
    bool                                                 clock_loaded_ = false;
    std::vector<spl::shared_ptr<core::video_channel>>    channels_;
    spl::shared_ptr<core::video_channel>                 channel_;
    spl::shared_ptr<core::cg_producer_registry>          cg_registry_;
    spl::shared_ptr<const core::frame_producer_registry> producer_registry_;

  public:
    command_context(const std::vector<spl::shared_ptr<core::video_channel>>&    channels,
                    const spl::shared_ptr<core::video_channel>&                 channel,
                    const spl::shared_ptr<core::cg_producer_registry>&          cg_registry,
                    const spl::shared_ptr<const core::frame_producer_registry>& producer_registry)
        : channels_(channels)
        , channel_(channel)
        , cg_registry_(cg_registry)
        , producer_registry_(producer_registry)
    {
    }

    void send_to_flash(const std::wstring& data)
    {
        if (!clock_loaded_) {
            core::frame_producer_dependencies dependencies(
                channel_->frame_factory(), channels_, channel_->video_format_desc(), producer_registry_, cg_registry_);
            cg_registry_
                ->get_or_create_proxy(channel_, dependencies, core::cg_proxy::DEFAULT_LAYER, L"hawrysklocka/clock")
                ->add(0, L"hawrysklocka/clock", true, L"", data);
            clock_loaded_ = true;
        } else {
            cg_registry_->get_proxy(channel_, core::cg_proxy::DEFAULT_LAYER)->update(0, data);
        }

        CASPAR_LOG(info) << L"CLK: Clockdata sent: " << data;
    }

    void reset()
    {
        channel_->stage()->clear(core::cg_proxy::DEFAULT_LAYER);
        clock_loaded_ = false;
        CASPAR_LOG(info) << L"CLK: Recieved and executed reset-command";
    }
};

template <class T>
T require_param(std::vector<std::wstring>::const_iterator&       params_current,
                const std::vector<std::wstring>::const_iterator& params_end,
                const std::string&                               param_name)
{
    if (params_current == params_end)
        throw std::runtime_error(param_name + " required");

    T value = boost::lexical_cast<T>(*params_current);

    ++params_current;

    return std::move(value);
}

std::wstring
get_xml(const std::wstring& command_name, bool has_clock_id, bool has_time, const std::vector<std::wstring>& parameters)
{
    std::wstringstream stream;

    stream << L"<templateData>";
    stream << L"<componentData id=\"command\">";
    stream << L"<command id=\"" << command_name << "\"";

    std::vector<std::wstring>::const_iterator it  = parameters.begin();
    std::vector<std::wstring>::const_iterator end = parameters.end();

    if (has_clock_id) {
        stream << L" clockID=\"" << require_param<int>(it, end, "clock id") << L"\"";
    }

    if (has_time) {
        stream << L" time=\"" << require_param<std::wstring>(it, end, "time") << L"\"";
    }

    bool has_parameters = it != end;

    stream << (has_parameters ? L">" : L" />");

    if (has_parameters) {
        for (; it != end; ++it) {
            stream << L"<parameter>" << (*it) << L"</parameter>";
        }

        stream << L"</command>";
    }

    stream << L"</componentData>";
    stream << L"</templateData>";

    return stream.str();
}

clk_command_handler create_send_xml_handler(const std::wstring&                     command_name,
                                            bool                                    expect_clock,
                                            bool                                    expect_time,
                                            const spl::shared_ptr<command_context>& context)
{
    return [=](const std::vector<std::wstring>& params) {
        context->send_to_flash(get_xml(command_name, expect_clock, expect_time, params));
    };
}

void add_command_handlers(clk_command_processor&                                      processor,
                          const std::vector<spl::shared_ptr<core::video_channel>>&    channels,
                          const spl::shared_ptr<core::video_channel>&                 channel,
                          const spl::shared_ptr<core::cg_producer_registry>&          cg_registry,
                          const spl::shared_ptr<const core::frame_producer_registry>& producer_registry)
{
    auto context = spl::make_shared<command_context>(channels, channel, cg_registry, producer_registry);

    processor.add_handler(L"DUR", create_send_xml_handler(L"DUR", true, true, context))
        .add_handler(L"NEWDUR", create_send_xml_handler(L"NEWDUR", true, true, context))
        .add_handler(L"UNTIL", create_send_xml_handler(L"UNTIL", true, true, context))
        .add_handler(L"NEXTEVENT", create_send_xml_handler(L"NEXTEVENT", true, false, context))
        .add_handler(L"STOP", create_send_xml_handler(L"STOP", true, false, context))
        .add_handler(L"ADD", create_send_xml_handler(L"ADD", true, true, context))
        .add_handler(L"SUB", create_send_xml_handler(L"SUB", true, true, context))
        .add_handler(L"TIMELINE_LOAD", create_send_xml_handler(L"TIMELINE_LOAD", false, false, context))
        .add_handler(L"TIMELINE_PLAY", create_send_xml_handler(L"TIMELINE_PLAY", false, false, context))
        .add_handler(L"TIMELINE_STOP", create_send_xml_handler(L"TIMELINE_STOP", false, false, context))
        .add_handler(L"RESET", [=](const std::vector<std::wstring>& params) { context->reset(); });
}

}}} // namespace caspar::protocol::CLK
