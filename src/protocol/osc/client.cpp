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

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

using namespace boost::asio::ip;

namespace caspar { namespace protocol { namespace osc {

template <typename T>
struct param_visitor : public boost::static_visitor<void>
{
    T& o;

    /// Binds argument conversion to the packet stream being measured or transmitted.
    param_visitor(T& o)
        : o(o)
    {
    }

    /// Preserves OSC boolean type tags rather than encoding integer flags.
    void operator()(const bool value) { o << value; }
    /// Encodes signed 32-bit monitor values as OSC integers.
    void operator()(const int32_t value) { o << static_cast<int32_t>(value); }
    /// Widens unsigned 32-bit values to avoid overflow in signed OSC integers.
    void operator()(const uint32_t value) { o << static_cast<int64_t>(value); }
    /// Preserves signed 64-bit monitor counters as OSC long integers.
    void operator()(const int64_t value) { o << static_cast<int64_t>(value); }
    /// Maps unsigned counters to OSC's signed 64-bit representation.
    void operator()(const uint64_t value) { o << static_cast<int64_t>(value); }
    /// Preserves single-precision arguments in the outgoing OSC message.
    void operator()(const float value) { o << value; }
    /// Narrows doubles to the single-precision format used by existing OSC telemetry.
    void operator()(const double value) { o << static_cast<float>(value); }
    /// Encodes narrow strings as null-terminated OSC string arguments.
    void operator()(const std::string& value) { o << value.c_str(); }
    /// Converts wide monitor strings to UTF-8 before OSC encoding.
    void operator()(const std::wstring& value) { o << u8(value).c_str(); }
};

/// Returns the current wall-clock time as an OSC/NTP 32-bit seconds and 32-bit fraction timestamp.
uint64_t osc_time_tag()
{
    using namespace std::chrono;

    constexpr uint64_t ntp_epoch_offset = 2208988800ULL;

    auto since_epoch = system_clock::now().time_since_epoch();
    auto secs        = duration_cast<seconds>(since_epoch);
    auto frac        = duration_cast<duration<double>>(since_epoch - secs).count();

    return ((static_cast<uint64_t>(secs.count()) + ntp_epoch_offset) << 32) |
           static_cast<uint64_t>(frac * 4294967296.0);
}

constexpr unsigned long max_bundle_size = 2048;

/// Uses identical encoding for message-size probing and actual packet construction.
void write_osc_message(::osc::OutboundPacketStream& stream, const std::string& address, const core::monitor::vector_t& args)
{
    stream << ::osc::BeginMessage(address.c_str());

    param_visitor<::osc::OutboundPacketStream> visitor(stream);
    for (const auto& element : args) {
        boost::apply_visitor(visitor, element);
    }

    stream << ::osc::EndMessage;
}

struct client::impl : public spl::enable_shared_from_this<client::impl>
{
    std::shared_ptr<boost::asio::io_context> io_context_;
    udp::socket                              socket_;
    std::map<udp::endpoint, int>             reference_counts_by_endpoint_;
    std::vector<char>                        buffer_;
    std::vector<char>                        probe_buffer_;

    std::mutex              mutex_;
    std::condition_variable cond_;
    core::monitor::state    bundle_;
    uint64_t                bundle_time_    = 0;
    bool                    bundle_pending_ = false;

    std::map<int, core::monitor::data_map_t> queued_transforms_;

    std::atomic<bool> abort_request_{false};
    std::thread       thread_;

  public:
    /// Opens the UDP socket and starts the worker that drains queued monitor snapshots.
    impl(std::shared_ptr<boost::asio::io_context> io_context)
        : io_context_(std::move(io_context))
        , socket_(*io_context_, udp::v4())
        , buffer_(1000000)
        , probe_buffer_(65536)
    {
        thread_ = std::thread([this] {
            try {
                std::map<udp::endpoint, boost::system::error_code> send_errors;
                while (!abort_request_) {
                    core::monitor::state       bundle;
                    uint64_t                   bundle_time;
                    std::vector<udp::endpoint> endpoints;

                    {
                        std::unique_lock<std::mutex> lock(mutex_);
                        cond_.wait(lock, [&] { return bundle_pending_ || abort_request_; });

                        if (abort_request_) {
                            return;
                        }

                        bundle          = std::move(bundle_);
                        bundle_         = core::monitor::state{};
                        bundle_time     = bundle_time_;
                        bundle_pending_ = false;

                        for (auto& p : reference_counts_by_endpoint_) {
                            endpoints.push_back(p.first);
                        }
                    }

                    std::erase_if(send_errors, [&](const auto& p) {
                        return !std::binary_search(endpoints.begin(), endpoints.end(), p.first);
                    });

                    if (endpoints.empty()) {
                        continue;
                    }

                    auto it = std::begin(bundle);

                    while (it != std::end(bundle)) {
                        ::osc::OutboundPacketStream o(reinterpret_cast<char*>(buffer_.data()),
                                                      static_cast<unsigned long>(buffer_.size()));

                        o << ::osc::BeginBundle(bundle_time);

                        // Measure each message's real encoded size before adding it, so one
                        // that wouldn't fit is deferred to the next bundle instead of silently
                        // overflowing this one. A message that alone exceeds the bundle limit
                        // is dropped outright so it can't stall the loop or blow the limit.
                        bool has_message = false;
                        while (it != std::end(bundle)) {
                            ::osc::OutboundPacketStream probe(probe_buffer_.data(),
                                                              static_cast<unsigned long>(probe_buffer_.size()));
                            try {
                                write_osc_message(probe, it->first, it->second);
                            } catch (const ::osc::OutOfBufferMemoryException&) {
                                CASPAR_LOG(warning)
                                    << "Dropping OSC message " << it->first << ": encoded size exceeds the "
                                    << probe_buffer_.size() << "-byte probe buffer";
                                ++it;
                                continue;
                            }

                            if (o.Size() + sizeof(uint32_t) + probe.Size() > max_bundle_size) {
                                if (!has_message) {
                                    CASPAR_LOG(warning)
                                        << "Dropping OSC message " << it->first << ": encoded size "
                                        << probe.Size() << " exceeds the " << max_bundle_size << "-byte bundle limit";
                                    ++it;
                                    continue;
                                }
                                break;
                            }

                            write_osc_message(o, it->first, it->second);
                            has_message = true;
                            ++it;
                        }

                        o << ::osc::EndBundle;

                        if (!has_message)
                            continue;

                        boost::system::error_code ec;
                        for (const auto& endpoint : endpoints) {
                            socket_.send_to(boost::asio::buffer(o.Data(), o.Size()), endpoint, 0, ec);
                            if (ec) {
                                const auto last = send_errors.find(endpoint);
                                if (last == send_errors.end() || last->second != ec)
                                    CASPAR_LOG(warning) << "OSC send to " << endpoint << " failed: " << ec.message();
                                send_errors[endpoint] = ec;
                                std::lock_guard<std::mutex> lock(mutex_);
                                queued_transforms_.clear();
                            } else if (send_errors.erase(endpoint)) {
                                CASPAR_LOG(info) << "OSC send to " << endpoint << " recovered";
                            }
                        }
                    }
                }
            } catch (...) {
                CASPAR_LOG_CURRENT_EXCEPTION();
            }
        });
    }

    /// Wakes and joins the sender before its socket and queued state are destroyed.
    ~impl()
    {
        abort_request_ = true;
        cond_.notify_all();
        thread_.join();
    }

    /// Keeps an endpoint subscribed for the token's lifetime and refreshes state for new endpoints.
    std::shared_ptr<void> get_subscription_token(const boost::asio::ip::udp::endpoint& endpoint)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (++reference_counts_by_endpoint_[endpoint] == 1) {
            queued_transforms_.clear();
        }

        std::weak_ptr<impl> weak_self = shared_from_this();

        return std::shared_ptr<void>(nullptr, [weak_self, endpoint](void*) {
            auto strong = weak_self.lock();

            if (!strong)
                return;

            auto& self = *strong;

            std::lock_guard<std::mutex> lock(self.mutex_);

            int reference_count_after = --self.reference_counts_by_endpoint_[endpoint];

            if (reference_count_after == 0) {
                self.reference_counts_by_endpoint_.erase(endpoint);
            }
        });
    }

    /// Merges a channel snapshot into the pending bundle before recording its transform deltas.
    void send(int channel_index, const core::monitor::state& state)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);

            if (reference_counts_by_endpoint_.empty())
                return;

            const auto                prefix   = "/channel/" + std::to_string(channel_index) + "/stage/layer/";
            auto&                     previous = queued_transforms_[channel_index];
            core::monitor::data_map_t current;

            // Compare only OSC transform properties, not renderer-only transform fields.
            // Commit the cache after merging into the queue, never while building stage state.
            for (const auto& p : state) {
                const auto layer_end    = p.first.find('/', prefix.size());
                const auto is_transform = p.first.starts_with(prefix) && layer_end != std::string::npos &&
                                          std::string_view(p.first).substr(layer_end).starts_with("/transform/");
                if (is_transform) {
                    current.emplace(p.first, p.second);
                    auto it = previous.find(p.first);
                    if (it != previous.end() && it->second == p.second)
                        continue;
                }
                bundle_[p.first] = p.second;
            }
            // Replacing this channel's cache also forgets layers absent from its snapshot.
            previous = std::move(current);

            if (!bundle_pending_) {
                bundle_time_    = osc_time_tag();
                bundle_pending_ = true;
            }
        }
        cond_.notify_all();
    }
};

/// Creates an independently queued OSC sender using the supplied I/O context.
client::client(std::shared_ptr<boost::asio::io_context> io_context)
    : impl_(new impl(std::move(io_context)))
{
}

/// Transfers the sender without restarting its worker or losing pending updates.
client::client(client&& other)
    : impl_(std::move(other.impl_))
{
}

/// Replaces this sender with the other's worker, subscriptions, and pending state.
client& client::operator=(client&& other)
{
    impl_ = std::move(other.impl_);
    return *this;
}

/// Releases ownership of the sender and its background worker.
client::~client() {}

/// Returns a lifetime token that shares the endpoint's subscription reference count.
std::shared_ptr<void> client::get_subscription_token(const boost::asio::ip::udp::endpoint& endpoint)
{
    return impl_->get_subscription_token(endpoint);
}

/// Queues applied channel telemetry without suppressing unchanged non-transform properties.
void client::send(int channel_index, const core::monitor::state& state) { impl_->send(channel_index, state); }

}}} // namespace caspar::protocol::osc
