/*
 * Copyright 2013 Sveriges Television AB http://casparcg.com/
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
 * Author: Helge Norberg, helge.norberg@svt.se
 */

#include "../StdAfx.h"

#include "client.h"

#include "oscpack/OscOutboundPacketStream.h"

#include <common/endian.h>
#include <common/utf.h>

#include <core/monitor/monitor.h>

#include <boost/asio.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

using namespace boost::asio::ip;

namespace caspar { namespace protocol { namespace osc {

template <typename T>
struct param_visitor : public boost::static_visitor<void>
{
    T& o;

    param_visitor(T& o)
        : o(o)
    {
    }

    void operator()(const bool value) { o << value; }
    void operator()(const int32_t value) { o << static_cast<int32_t>(value); }
    void operator()(const uint32_t value) { o << static_cast<int64_t>(value); }
    void operator()(const int64_t value) { o << static_cast<int64_t>(value); }
    void operator()(const uint64_t value) { o << static_cast<int64_t>(value); }
    void operator()(const float value) { o << value; }
    void operator()(const double value) { o << static_cast<float>(value); }
    void operator()(const std::string& value) { o << value.c_str(); }
    void operator()(const std::wstring& value) { o << u8(value).c_str(); }
};

struct client::impl : public spl::enable_shared_from_this<client::impl>
{
    // How often to poll diagnostics_provider_ and broadcast its result to diagnostics-subscribed
    // endpoints. Diagnostics telemetry isn't tied to any channel's tick (see core::diagnostics::osc_graph),
    // so unlike the regular per-channel bundle_ it needs its own cadence - this roughly matches the SFML
    // osd window's own 40ms render loop. Only incurred while diagnostics_reference_counts_by_endpoint_ is
    // non-empty; otherwise the thread simply blocks on cond_ as before.
    static constexpr std::chrono::milliseconds diagnostics_push_interval_{40};

    std::shared_ptr<boost::asio::io_context> io_context_;
    udp::socket                              socket_;
    std::map<udp::endpoint, int>             reference_counts_by_endpoint_;
    std::map<udp::endpoint, int>             diagnostics_reference_counts_by_endpoint_;
    std::function<core::monitor::state()>    diagnostics_provider_;
    std::vector<char>                        buffer_;

    std::mutex              mutex_;
    std::condition_variable cond_;
    core::monitor::state    bundle_;
    uint64_t                bundle_time_ = 0;

    uint64_t time_ = 0;

    std::atomic<bool> abort_request_{false};
    std::thread       thread_;

  public:
    impl(std::shared_ptr<boost::asio::io_context> io_context,
         std::function<core::monitor::state()>    diagnostics_provider)
        : io_context_(std::move(io_context))
        , socket_(*io_context_, udp::v4())
        , diagnostics_provider_(std::move(diagnostics_provider))
        , buffer_(1000000)
    {
        thread_ = std::thread([this] {
            try {
                auto next_diagnostics_deadline = std::chrono::steady_clock::time_point{};
                while (!abort_request_) {
                    core::monitor::state       bundle;
                    uint64_t                   bundle_time      = 0;
                    uint64_t                   diagnostics_time = 0;
                    bool                       have_bundle      = false;
                    bool                       send_diagnostics = false;
                    std::vector<udp::endpoint> endpoints;
                    std::vector<udp::endpoint> diagnostics_endpoints;

                    {
                        std::unique_lock<std::mutex> lock(mutex_);

                        if (!diagnostics_provider_ || diagnostics_reference_counts_by_endpoint_.empty()) {
                            next_diagnostics_deadline = {};
                            cond_.wait(lock, [&] {
                                return bundle_time_ > 0 || abort_request_ ||
                                       (diagnostics_provider_ && !diagnostics_reference_counts_by_endpoint_.empty());
                            });
                        } else {
                            if (next_diagnostics_deadline == std::chrono::steady_clock::time_point{}) {
                                next_diagnostics_deadline =
                                    std::chrono::steady_clock::now() + diagnostics_push_interval_;
                            }
                            cond_.wait_until(lock, next_diagnostics_deadline, [&] {
                                return bundle_time_ > 0 || abort_request_ ||
                                       diagnostics_reference_counts_by_endpoint_.empty();
                            });
                        }

                        if (abort_request_) {
                            return;
                        }

                        have_bundle = bundle_time_ > 0;
                        if (have_bundle) {
                            bundle       = std::move(bundle_);
                            bundle_time  = bundle_time_;
                            bundle_time_ = 0;
                        }

                        for (auto& p : reference_counts_by_endpoint_) {
                            endpoints.push_back(p.first);
                        }
                        for (auto& p : diagnostics_reference_counts_by_endpoint_) {
                            diagnostics_endpoints.push_back(p.first);
                        }

                        const auto now = std::chrono::steady_clock::now();
                        if (diagnostics_provider_ && !diagnostics_endpoints.empty() &&
                            next_diagnostics_deadline == std::chrono::steady_clock::time_point{}) {
                            next_diagnostics_deadline = now + diagnostics_push_interval_;
                        }
                        if (diagnostics_provider_ && !diagnostics_endpoints.empty() &&
                            now >= next_diagnostics_deadline) {
                            send_diagnostics = true;
                            diagnostics_time = time_++;
                        }
                    }

                    if (have_bundle && !endpoints.empty()) {
                        send_bundle(bundle, bundle_time, endpoints);
                    }

                    if (send_diagnostics) {
                        send_bundle(diagnostics_provider_(), diagnostics_time, diagnostics_endpoints);
                        next_diagnostics_deadline =
                            std::max(next_diagnostics_deadline + diagnostics_push_interval_,
                                     std::chrono::steady_clock::now() + diagnostics_push_interval_);
                    }
                }
            } catch (...) {
                CASPAR_LOG_CURRENT_EXCEPTION();
            }
        });
    }

    // Splits state across as many OSC bundles as needed (oscpack has no streaming API, so each bundle is
    // built into buffer_ up front) and sends each to every one of endpoints.
    void send_bundle(const core::monitor::state& state, uint64_t time, const std::vector<udp::endpoint>& endpoints)
    {
        auto it = std::begin(state);

        while (it != std::end(state)) {
            ::osc::OutboundPacketStream o(reinterpret_cast<char*>(buffer_.data()),
                                          static_cast<unsigned long>(buffer_.size()));

            o << ::osc::BeginBundle(time);

            // TODO (fix): < 2048 is a hack. Properly calculate if messages will fit.
            while (it != std::end(state) && o.Size() < 2048) {
                o << ::osc::BeginMessage(it->first.c_str());

                param_visitor<decltype(o)> param_visitor(o);
                for (const auto& element : it->second) {
                    boost::apply_visitor(param_visitor, element);
                }

                o << ::osc::EndMessage;

                ++it;
            }

            o << ::osc::EndBundle;

            boost::system::error_code ec;
            for (const auto& endpoint : endpoints) {
                socket_.send_to(boost::asio::buffer(o.Data(), o.Size()), endpoint, 0, ec);
            }
        }
    }

    ~impl()
    {
        abort_request_ = true;
        cond_.notify_all();
        thread_.join();
    }

    // TODO (refactor) This is weird...
    std::shared_ptr<void> get_subscription_token(const boost::asio::ip::udp::endpoint& endpoint,
                                                 bool                                  include_diagnostics)
    {
        bool wake_diagnostics_worker = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);

            ++reference_counts_by_endpoint_[endpoint];
            if (include_diagnostics) {
                wake_diagnostics_worker = diagnostics_reference_counts_by_endpoint_.empty();
                ++diagnostics_reference_counts_by_endpoint_[endpoint];
            }
        }
        if (wake_diagnostics_worker)
            cond_.notify_all();

        std::weak_ptr<impl> weak_self = shared_from_this();

        return std::shared_ptr<void>(nullptr, [weak_self, endpoint, include_diagnostics](void*) {
            auto strong = weak_self.lock();

            if (!strong)
                return;

            auto& self                    = *strong;
            bool  wake_diagnostics_worker = false;
            {
                std::lock_guard<std::mutex> lock(self.mutex_);

                int reference_count_after = --self.reference_counts_by_endpoint_[endpoint];

                if (reference_count_after == 0) {
                    self.reference_counts_by_endpoint_.erase(endpoint);
                }

                if (include_diagnostics) {
                    int diagnostics_reference_count_after = --self.diagnostics_reference_counts_by_endpoint_[endpoint];

                    if (diagnostics_reference_count_after == 0) {
                        self.diagnostics_reference_counts_by_endpoint_.erase(endpoint);
                        wake_diagnostics_worker = self.diagnostics_reference_counts_by_endpoint_.empty();
                    }
                }
            }
            if (wake_diagnostics_worker)
                self.cond_.notify_all();
        });
    }

    void send(const core::monitor::state& state)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);

            // TODO: time_++ is a hack. Use proper channel time.
            bundle_time_ = time_++;
            bundle_      = state;
        }
        cond_.notify_all();
    }
};

client::client(std::shared_ptr<boost::asio::io_context> io_context,
               std::function<core::monitor::state()>    diagnostics_provider)
    : impl_(new impl(std::move(io_context), std::move(diagnostics_provider)))
{
}

client::client(client&& other)
    : impl_(std::move(other.impl_))
{
}

client& client::operator=(client&& other)
{
    impl_ = std::move(other.impl_);
    return *this;
}

client::~client() {}

std::shared_ptr<void> client::get_subscription_token(const boost::asio::ip::udp::endpoint& endpoint,
                                                     bool                                  include_diagnostics)
{
    return impl_->get_subscription_token(endpoint, include_diagnostics);
}

void client::send(const core::monitor::state& state) { impl_->send(state); }

}}} // namespace caspar::protocol::osc
