#include "client.h"
#include "oscpack/OscReceivedElements.h"

#include <common/diagnostics/graph.h>
#include <core/frame/frame_transform.h>
#include <core/mixer/image/image_mixer.h>
#include <core/mixer/mixer.h>
#include <core/producer/stage.h>
#include <core/video_channel.h>

#include <boost/asio.hpp>
#include <boost/log/core.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/sinks/sync_frontend.hpp>
#include <boost/log/sinks/text_ostream_backend.hpp>
#include <boost/make_shared.hpp>

#include <array>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <iterator>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace monitor = caspar::core::monitor;
using boost::asio::ip::udp;
using caspar::protocol::osc::client;
using values_t = std::map<std::string, std::vector<float>>;

/// Fails the test with a diagnostic that remains enabled in release builds.
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

/// Builds the default eleven-property snapshot with the expected scalar and vector shapes.
monitor::state transform()
{
    monitor::state state;
    state["opacity"]          = 1.0;
    state["contrast"]         = 1.0;
    state["brightness"]       = 1.0;
    state["saturation"]       = 1.0;
    state["anchor"]           = {0.0, 0.0};
    state["fill_translation"] = {0.0, 0.0};
    state["fill_scale"]       = {1.0, 1.0};
    state["clip_translation"] = {0.0, 0.0};
    state["clip_scale"]       = {1.0, 1.0};
    state["angle"]            = 0.0;
    state["volume"]           = 1.0;
    return state;
}

/// Selects the test layer while allowing channel-isolation checks to reuse its addresses.
std::string layer_path(int channel) { return "/channel/" + std::to_string(channel) + "/stage/layer/10"; }

/// Combines transform telemetry, an unsuppressed producer property, and a tick completion marker.
monitor::state snapshot(int channel, const monitor::state& properties, std::int32_t tick)
{
    monitor::state state;
    state[layer_path(channel)]["transform"] = properties;
    // Producer properties with a nested "transform" must retain their existing cadence.
    state[layer_path(channel)]["foreground"]["transform"]["opacity"] = 0.75;
    state["/test/tick"]                                              = tick;
    return state;
}

/// Collects split UDP bundles through the requested tick while checking encoding and size limits.
values_t receive(udp::socket& socket, std::int32_t tick)
{
    values_t                values;
    const auto              deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::array<char, 65536> buffer;
    while (std::chrono::steady_clock::now() < deadline) {
        boost::system::error_code ec;
        const auto                size = socket.receive(boost::asio::buffer(buffer), 0, ec);
        if (ec == boost::asio::error::would_block || ec == boost::asio::error::try_again) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if (ec)
            throw boost::system::system_error(ec);

        require(size <= 2048, "OSC bundle exceeded 2048 bytes");
        ::osc::ReceivedBundle bundle(::osc::ReceivedPacket(buffer.data(), static_cast<::osc::int32>(size)));
        require(bundle.TimeTag() != 0, "OSC bundle has a zero timestamp");
        bool complete = false;
        for (auto it = bundle.ElementsBegin(); it != bundle.ElementsEnd(); ++it) {
            ::osc::ReceivedMessage message(*it);
            const std::string      address = message.AddressPattern();
            if (address == "/test/tick") {
                require(message.ArgumentCount() == 1, "Tick must have one argument");
                complete = message.ArgumentsBegin()->AsInt32() == tick;
                continue;
            }
            std::vector<float> args;
            for (auto arg = message.ArgumentsBegin(); arg != message.ArgumentsEnd(); ++arg) {
                if (arg->IsFloat())
                    args.push_back(arg->AsFloat());
            }
            if (address.find("/transform/") != std::string::npos)
                require(args.size() == message.ArgumentCount(), "Transform arguments must all be floats");
            values[address] = std::move(args);
        }
        if (complete)
            return values;
    }
    throw std::runtime_error("Timed out waiting for OSC tick " + std::to_string(tick));
}

/// Checks the exact transform address set, argument counts, and transmitted float values.
void require_transform(const values_t& received, int channel, const monitor::state& expected)
{
    const auto  prefix = layer_path(channel) + "/transform/";
    std::size_t count  = 0;
    for (const auto& p : received) {
        if (p.first.starts_with(prefix))
            ++count;
    }
    require(count == static_cast<std::size_t>(std::distance(expected.begin(), expected.end())),
            "Unexpected number of transform properties");
    for (const auto& p : expected) {
        const auto it = received.find(prefix + p.first);
        require(it != received.end(), "Missing property " + p.first);
        std::vector<float> args;
        for (const auto& arg : p.second)
            args.push_back(static_cast<float>(boost::get<double>(arg)));
        require(it->second == args, "Wrong OSC argument values or shape for " + p.first);
    }
}

class captured_logs
{
    using sink_t = boost::log::sinks::synchronous_sink<boost::log::sinks::wtext_ostream_backend>;
    boost::shared_ptr<std::wostringstream> stream_ = boost::make_shared<std::wostringstream>();
    boost::shared_ptr<sink_t>              sink_   = boost::make_shared<sink_t>();

  public:
    /// Attaches a synchronous sink so sender diagnostics can be counted without timing guesses.
    captured_logs()
    {
        sink_->locked_backend()->add_stream(stream_);
        sink_->set_formatter(boost::log::expressions::stream << boost::log::expressions::wmessage);
        boost::log::core::get()->add_sink(sink_);
    }
    /// Detaches the test sink without changing other logging destinations.
    ~captured_logs() { boost::log::core::get()->remove_sink(sink_); }

    /// Counts matching diagnostics while excluding concurrent writes from the sender.
    std::size_t count(const std::wstring& text)
    {
        auto        lock     = sink_->locked_backend();
        const auto  contents = stream_->str();
        std::size_t count    = 0;
        for (auto pos = contents.find(text); pos != std::wstring::npos; pos = contents.find(text, pos + text.size()))
            ++count;
        return count;
    }
};

/// Verifies oversized-message survival, repeated-error suppression, and endpoint-history cleanup.
void test_sender_errors()
{
    captured_logs logs;
    auto          context = std::make_shared<boost::asio::io_context>();
    udp::socket   socket(*context, udp::endpoint(boost::asio::ip::address_v4::loopback(), 0));
    socket.non_blocking(true);
    client       sender(context);
    auto         token      = sender.get_subscription_token(socket.local_endpoint());
    auto         properties = transform();
    std::int32_t tick       = 0;

    auto oversized            = snapshot(1, properties, ++tick);
    oversized["/a/oversized"] = std::string(70000, 'x');
    sender.send(1, oversized);
    const auto received = receive(socket, tick);
    require(!received.contains("/a/oversized"), "Oversized OSC message was not dropped");
    require_transform(received, 1, properties);
    sender.send(1, snapshot(1, properties, ++tick));
    receive(socket, tick);
    require(logs.count(L"Dropping OSC message /a/oversized") == 1, "Oversized message was not explicitly logged once");

    monitor::state oversized_only;
    oversized_only["/z/oversized"] = std::string(70000, 'x');
    sender.send(1, oversized_only);
    sender.send(1, snapshot(1, properties, ++tick));
    receive(socket, tick);
    sender.send(1, snapshot(1, properties, ++tick));
    receive(socket, tick);
    require(logs.count(L"Dropping OSC message /z/oversized") == 1, "Trailing oversized message stopped the sender");

    // A message smaller than the probe buffer but larger than the bundle limit must still be
    // dropped on its own, rather than being written out as a bundle exceeding max_bundle_size
    // (receive() below enforces the 2048-byte limit on every datagram it reads).
    monitor::state bundle_oversized_only;
    bundle_oversized_only["/y/bundle_oversized"] = std::string(3000, 'x');
    sender.send(1, bundle_oversized_only);
    sender.send(1, snapshot(1, properties, ++tick));
    receive(socket, tick);
    require(logs.count(L"Dropping OSC message /y/bundle_oversized") == 1,
            "Message exceeding the bundle limit alone was not dropped");

    auto invalid_token = sender.get_subscription_token(udp::endpoint(boost::asio::ip::address_v4::loopback(), 0));
    for (int i = 0; i < 20; ++i) {
        sender.send(1, snapshot(1, properties, ++tick));
        require_transform(receive(socket, tick), 1, properties);
    }
    require(logs.count(L"failed:") == 1, "Repeated identical OSC send errors flooded the log");
    invalid_token.reset();
    for (int i = 0; i < 2; ++i) {
        sender.send(1, snapshot(1, properties, ++tick));
        receive(socket, tick);
    }
    invalid_token = sender.get_subscription_token(udp::endpoint(boost::asio::ip::address_v4::loopback(), 0));
    sender.send(1, snapshot(1, properties, ++tick));
    receive(socket, tick);
    require(logs.count(L"failed:") == 2, "Removed endpoint retained stale send-error history");
}

class test_image_mixer : public caspar::core::image_mixer
{
  public:
    bool         fail_render = false;
    std::uint8_t marker      = 0;

    /// Encodes opacity into a pixel marker to identify the transform used for rendering.
    void push(const caspar::core::frame_transform& value) override
    {
        marker = static_cast<std::uint8_t>(value.image_transform.opacity * 128);
    }
    /// Ignores source pixels because this test renderer produces transform markers instead.
    void visit(const caspar::core::const_frame&) override {}
    /// Leaves the marker intact because test inputs contain no nested render layers.
    void pop() override {}
    /// Ignores aspect ratio because the synthetic marker has no spatial geometry.
    void update_aspect_ratio(double) override {}

    /// Returns a deferred marker image or the injected failure for this render submission.
    std::future<std::tuple<caspar::array<const std::uint8_t>, std::shared_ptr<caspar::core::texture>>>
    render(const caspar::core::video_format_desc&) override
    {
        return std::async(std::launch::deferred, [value = marker, fail = fail_render] {
            if (fail)
                throw std::runtime_error("Injected render failure");
            return std::make_tuple(caspar::array<const std::uint8_t>(std::vector<std::uint8_t>{value}),
                                   std::shared_ptr<caspar::core::texture>{});
        });
    }

    /// Rejects unexpected producer allocations in tests that supply their own draw frames.
    caspar::core::mutable_frame create_frame(const void*, const caspar::core::pixel_format_desc&) override
    {
        throw std::logic_error("Unexpected frame allocation");
    }
    /// Rejects depth-specific allocations that would bypass the synthetic render path.
    caspar::core::mutable_frame
    create_frame(const void*, const caspar::core::pixel_format_desc&, caspar::common::bit_depth) override
    {
        throw std::logic_error("Unexpected frame allocation");
    }
#ifdef WIN32
    /// Rejects GPU imports because the regression renderer is deliberately hardware-independent.
    caspar::core::const_frame import_d3d_texture(const void*,
                                                 const std::shared_ptr<caspar::accelerator::d3d::d3d_texture2d>&,
                                                 caspar::core::pixel_format,
                                                 caspar::common::bit_depth) override
    {
        throw std::logic_error("Unexpected texture import");
    }
#endif
    /// Matches the byte-sized marker image returned by the synthetic renderer.
    caspar::common::bit_depth depth() const override { return caspar::common::bit_depth::bit8; }
};

/// Matches progressive and interlaced render markers to status and checks failed renders cannot advance it.
void test_mixer_alignment()
{
    namespace core = caspar::core;
    for (auto format : {core::video_format::x720p5000, core::video_format::x1080i5000}) {
        const auto  desc  = core::video_format_repository().find_format(format);
        auto        image = caspar::spl::make_shared<test_image_mixer>();
        core::mixer mixer(1, caspar::spl::make_shared<caspar::diagnostics::graph>(), image);
        for (int field = 0; field < 12; ++field) {
            core::draw_frame draw;
            draw.transform().image_transform.opacity = static_cast<double>(field + 1) / 16;
            monitor::state properties;
            properties["opacity"] = draw.transform().image_transform.opacity;
            auto frame            = mixer({draw}, desc, desc.audio_cadence.front(), properties);
            if (field < desc.field_count) {
                require(!frame, "Mixer returned a frame before buffer priming");
                const auto state = mixer.transforms();
                require(state.begin() == state.end(), "Transform status was exposed before a frame was rendered");
            } else {
                require(static_cast<bool>(frame), "Mixer did not return a rendered frame");
                const auto state    = mixer.transforms();
                const auto expected = static_cast<double>(field + 1 - desc.field_count) / 16;
                require(boost::get<double>(state.begin()->second.front()) == expected,
                        "Transform status leads or lags its rendered frame");
                require(frame.image_data(0).data()[0] == static_cast<std::uint8_t>(expected * 128),
                        "Transform status does not match the actual rendered frame");
            }
        }
        image->fail_render = true;
        bool failed        = false;
        for (int i = 0; i <= desc.field_count; ++i) {
            const auto                last = mixer.transforms();
            const monitor::data_map_t previous(last.begin(), last.end());
            try {
                mixer({}, desc, desc.audio_cadence.front(), {});
            } catch (const std::runtime_error& e) {
                require(std::string(e.what()) == "Injected render failure", "Unexpected render error");
                failed           = true;
                const auto state = mixer.transforms();
                require(monitor::data_map_t(state.begin(), state.end()) == previous,
                        "Failed rendering advanced the applied transform state");
                break;
            }
        }
        require(failed, "Injected render failure was not observed");
    }
}

/// Verifies stage-to-OSC updates wait for rendering and survive skipped forwarding during tweens.
void test_stage_snapshots()
{
    namespace core = caspar::core;
    core::stage stage(1,
                      caspar::spl::make_shared<caspar::diagnostics::graph>(),
                      core::video_format_repository().find_format(core::video_format::x720p5000));
    stage.pause(10).get();
    core::mixer mixer(
        1, caspar::spl::make_shared<caspar::diagnostics::graph>(), caspar::spl::make_shared<test_image_mixer>());

    auto        context = std::make_shared<boost::asio::io_context>();
    udp::socket socket(*context, udp::endpoint(boost::asio::ip::address_v4::loopback(), 0));
    socket.non_blocking(true);
    client           sender(context);
    auto             token = sender.get_subscription_token(socket.local_endpoint());
    std::uint64_t    frame = 0;
    std::vector<int> backgrounds;
    auto             produce = [&] {
        const auto frames = stage(++frame, backgrounds, [](int, const core::layer_frame&) {});
        mixer(frames.frames, frames.format_desc, frames.nb_samples, frames.transforms);
        monitor::state state;
        state[""]["channel"][1]["stage"] = stage.state();
        state[""]["channel"][1]["stage"] = mixer.transforms();
        state["/test/tick"]              = static_cast<std::int32_t>(frame);
        return state;
    };
    auto send = [&] {
        sender.send(1, produce());
        return receive(socket, static_cast<std::int32_t>(frame));
    };

    require_transform(send(), 1, {});
    require_transform(send(), 1, transform());
    require_transform(send(), 1, {});
    stage
        .apply_transform(
            10,
            [](core::frame_transform value) {
                value.image_transform.opacity = 0.5;
                return value;
            },
            0,
            caspar::tweener())
        .get();
    require_transform(send(), 1, {}); // New transform is still in the mixer buffer.
    produce();                        // Render it, but simulate a channel tick that fails before OSC submission.
    monitor::state change;
    change["opacity"] = 0.5;
    require_transform(send(), 1, change);
    require_transform(send(), 1, {});

    stage
        .apply_transform(
            10,
            [](core::frame_transform value) {
                value.image_transform.enable_geometry_modifiers = true;
                return value;
            },
            0,
            caspar::tweener())
        .get();
    require_transform(send(), 1, {});

    stage
        .apply_transform(
            10,
            [](core::frame_transform value) {
                value.image_transform.angle = 1.0;
                return value;
            },
            4,
            caspar::tweener())
        .get();
    require_transform(send(), 1, {});
    for (int step = 1; step <= 4; ++step) {
        change          = {};
        change["angle"] = static_cast<double>(step) / 4;
        require_transform(send(), 1, change);
    }
    require_transform(send(), 1, {});
}

/// Ensures a ticking channel with no consumers never announces transforms it has not mixed.
void test_unmixed_status()
{
    namespace core = caspar::core;
    std::mutex              mutex;
    std::condition_variable changed;
    int                     ticks               = 0;
    bool                    announced_transform = false;
    core::video_channel     channel(1,
                                core::video_format_repository().find_format(core::video_format::x720p5000),
                                core::color_space::bt709,
                                std::make_unique<test_image_mixer>(),
                                [&](monitor::state state) {
                                    std::lock_guard<std::mutex> lock(mutex);
                                    for (const auto& p : state) {
                                        if (p.first.find("/transform/") != std::string::npos)
                                            announced_transform = true;
                                    }
                                    ++ticks;
                                    changed.notify_all();
                                });
    channel.stage()->pause(10).get();
    channel.stage()
        ->apply_transform(
            10,
            [](core::frame_transform value) {
                value.image_transform.opacity = 0.5;
                return value;
            },
            0,
            caspar::tweener())
        .get();
    std::unique_lock<std::mutex> lock(mutex);
    const auto                   target = ticks + 3;
    require(changed.wait_for(lock, std::chrono::seconds(5), [&] { return ticks >= target; }),
            "Channel stopped ticking without consumers");
    require(!announced_transform, "Channel announced a transform without ever mixing a frame");
}

/// Exercises frame timing, subscriber lifecycle, channel isolation, coalescing, and bundle splitting.
void run()
{
    test_sender_errors();
    test_stage_snapshots();
    test_mixer_alignment();
    test_unmixed_status();
    auto        context = std::make_shared<boost::asio::io_context>();
    udp::socket first(*context, udp::endpoint(boost::asio::ip::address_v4::loopback(), 0));
    first.non_blocking(true);
    client       sender(context);
    auto         properties = transform();
    std::int32_t tick       = 0;
    auto         send       = [&](int channel, const monitor::state& value) {
        sender.send(channel, snapshot(channel, value, ++tick));
        return receive(first, tick);
    };

    sender.send(1, snapshot(1, properties, ++tick));
    auto first_token = sender.get_subscription_token(first.local_endpoint());
    require_transform(send(1, properties), 1, properties);
    auto idle = send(1, properties);
    require_transform(idle, 1, {});
    require(idle.contains(layer_path(1) + "/foreground/transform/opacity"),
            "Unrelated producer telemetry was suppressed");

    // Exercise every scalar/vector property without changing any renderer-only fields.
    for (const auto& p : transform()) {
        monitor::state change;
        if (p.second.size() == 2)
            change[p.first] = {0.25, 0.5};
        else
            change[p.first] = 0.25;
        properties[p.first] = change.begin()->second;
        require_transform(send(1, properties), 1, change);
        require_transform(send(1, properties), 1, {});
    }

    require_transform(send(2, properties), 2, properties);
    require_transform(send(1, properties), 1, {});

    monitor::state removed;
    removed["/test/tick"] = ++tick;
    sender.send(1, removed);
    receive(first, tick);
    require_transform(send(1, properties), 1, properties);
    require_transform(send(2, properties), 2, {});

    auto duplicate_token = sender.get_subscription_token(first.local_endpoint());
    require_transform(send(1, properties), 1, {});

    udp::socket second(*context, udp::endpoint(boost::asio::ip::address_v4::loopback(), 0));
    second.non_blocking(true);
    auto second_token = sender.get_subscription_token(second.local_endpoint());
    require_transform(send(1, properties), 1, properties);
    require_transform(receive(second, tick), 1, properties);
    require_transform(send(1, properties), 1, {});
    require_transform(receive(second, tick), 1, {});
    require_transform(send(2, properties), 2, properties);
    require_transform(receive(second, tick), 2, properties);
    second_token.reset();

    // Pending deltas must survive subsequent unchanged snapshots and other channels.
    for (int i = 0; i < 20; ++i) {
        properties["opacity"] = static_cast<double>(i) / 32;
        sender.send(1, snapshot(1, properties, ++tick));
    }
    sender.send(2, snapshot(2, properties, ++tick));
    sender.send(1, snapshot(1, properties, ++tick));
    const auto merged = receive(first, tick);
    require(merged.at(layer_path(1) + "/transform/opacity") == std::vector<float>{19.0f / 32},
            "Latest pending transform was lost");
    require_transform(send(1, properties), 1, {});

    first_token.reset();
    duplicate_token.reset();
    sender.send(1, snapshot(1, properties, ++tick));
    first_token = sender.get_subscription_token(first.local_endpoint());
    require_transform(send(1, properties), 1, properties);

    // Port zero fails locally. It sorts before the live endpoint, so receiving the
    // tick also confirms that the sender has invalidated its cache after the error.
    auto invalid_token = sender.get_subscription_token(udp::endpoint(boost::asio::ip::address_v4::loopback(), 0));
    require_transform(send(1, properties), 1, properties);
    require_transform(send(1, properties), 1, properties);
    invalid_token.reset();
    require_transform(send(1, properties), 1, properties);
    require_transform(send(1, properties), 1, {});

    // Force multiple bundles and verify that no transform message is dropped.
    monitor::state large;
    for (int layer = 0; layer < 40; ++layer)
        large[""]["channel"][3]["stage"]["layer"][layer]["transform"] = properties;
    large["/test/tick"] = ++tick;
    sender.send(3, large);
    const auto split = receive(first, tick);
    require(split.size() == 40 * 11, "Split OSC bundles lost transform messages");
}

} // namespace

/// Converts regression failures into a nonzero CTest exit code with a readable diagnostic.
int main()
{
    try {
        run();
        std::cout << "OSC transform queue and UDP loopback tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "OSC test failed: " << e.what() << '\n';
        return 1;
    }
}
