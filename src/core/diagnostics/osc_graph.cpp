/*
 * Copyright (c) 2026 Sveriges Television AB <info@casparcg.com>
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
 */

#include "../StdAfx.h"

#include "osc_graph.h"

#include "call_context.h"

#include <common/diagnostics/graph.h>
#include <common/memory.h>
#include <common/utf.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace caspar::core::diagnostics::osc_graph {

namespace {

const char* tag_severity_name(caspar::diagnostics::tag_severity severity)
{
    switch (severity) {
        case caspar::diagnostics::tag_severity::WARNING:
            return "warning";
        case caspar::diagnostics::tag_severity::INFO:
            return "info";
        default:
            return "silent";
    }
}

std::atomic<long long> g_next_id{1};

// Mirrors every value/text/color/tag set on its owning caspar::diagnostics::graph into plain members
// guarded by a mutex, so a state snapshot can be built at any time from any thread (protocol::osc::client's
// background thread). Captures the same per-thread channel/layer call_context the SFML osd_graph shows in
// its corner, and a stable id (rather than registry position) so clients can track a graph across
// snapshots even as other graphs come and go.
class osc_graph_sink : public caspar::diagnostics::spi::graph_sink
{
    const long long                               id_      = g_next_id.fetch_add(1, std::memory_order_relaxed);
    const caspar::core::diagnostics::call_context context_ = caspar::core::diagnostics::call_context::for_thread();

    mutable std::mutex                      mutex_;
    std::string                             text_;
    std::unordered_map<std::string, double> values_;
    std::unordered_map<std::string, int>    colors_;
    std::unordered_map<std::string, int>    pending_tags_; // name -> tag_severity, cleared once emitted

  public:
    void activate() override {}

    void set_text(const std::wstring& value) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        text_ = u8(value);
    }

    void set_value(const std::string& name, double value) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        values_[name] = value;
    }

    void set_color(const std::string& name, int color) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        colors_[name] = color;
    }

    void set_tag(caspar::diagnostics::tag_severity severity, const std::string& name) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_tags_[name] = static_cast<int>(severity);
    }

    // Preserve the explicit reset behavior exposed by caspar::diagnostics::graph.
    void auto_reset() override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& p : values_)
            p.second = 0.0;
    }

    void write_state(core::monitor::state& state)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        // Root at "" so the resulting OSC addresses start with "/" (same convention as the channel state).
        auto node       = state[""]["diag"][id_];
        node["channel"] = context_.video_channel;
        node["layer"]   = context_.layer;
        node["text"]    = text_;

        for (auto& p : values_)
            node["value"][p.first] = p.second;

        for (auto& p : colors_)
            node["color"][p.first] = p.second;

        for (auto& p : pending_tags_)
            node["tag"][p.first] =
                std::string(tag_severity_name(static_cast<caspar::diagnostics::tag_severity>(p.second)));

        pending_tags_.clear();
    }
};

std::mutex                                 g_registry_mutex;
std::vector<std::weak_ptr<osc_graph_sink>> g_registry;

} // namespace

void register_sink()
{
    caspar::diagnostics::spi::register_sink_factory([]() -> spl::shared_ptr<caspar::diagnostics::spi::graph_sink> {
        auto sink = std::make_shared<osc_graph_sink>();

        {
            std::lock_guard<std::mutex> lock(g_registry_mutex);
            g_registry.erase(std::remove_if(g_registry.begin(),
                                            g_registry.end(),
                                            [](const std::weak_ptr<osc_graph_sink>& w) { return w.expired(); }),
                             g_registry.end());
            g_registry.push_back(sink);
        }

        return spl::make_shared_ptr(std::static_pointer_cast<caspar::diagnostics::spi::graph_sink>(sink));
    });
}

core::monitor::state snapshot_state()
{
    std::vector<std::shared_ptr<osc_graph_sink>> live;
    {
        std::lock_guard<std::mutex> lock(g_registry_mutex);
        g_registry.erase(std::remove_if(g_registry.begin(),
                                        g_registry.end(),
                                        [&](const std::weak_ptr<osc_graph_sink>& w) {
                                            auto s = w.lock();
                                            if (s)
                                                live.push_back(s);
                                            return !s;
                                        }),
                         g_registry.end());
    }

    core::monitor::state state;
    for (auto& s : live)
        s->write_state(state);
    return state;
}

} // namespace caspar::core::diagnostics::osc_graph
