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
 *         James Wise, james.wise@bluefish444.com
 */

#include "../StdAfx.h"

#include "../util/blue_velvet.h"
#include "../util/memory.h"

#include <core/consumer/channel_info.h>
#include <core/consumer/frame_consumer.h>
#include <core/frame/frame.h>

#include <common/diagnostics/graph.h>
#include <common/executor.h>
#include <common/param.h>
#include <common/timer.h>

#include <boost/property_tree/ptree.hpp>
#include <boost/range/algorithm.hpp>

#include <atomic>
#include <condition_variable>
#include <future>
#include <mutex>

namespace caspar { namespace bluefish {

#define BLUEFISH_MAX_SOFTWARE_BUFFERS 4
#define SIZE_TEMP_AUDIO_BUFFER                                                                                         \
    (2048 * 16) // max 2002 samples across 16 channels, use 2048 for safety cos sometimes caspar gives us too many...

enum class hardware_downstream_keyer_mode
{
    disable  = 0,
    external = 1,
    internal = 2, // Bluefish dedicated HW keyer - only available on some models.
};

enum class hardware_downstream_keyer_audio_source
{
    SDIVideoInput      = 1,
    VideoOutputChannel = 2
};

enum class bluefish_hardware_output_channel
{
    channel_1 = 1,
    channel_2 = 2,
    channel_3 = 3,
    channel_4 = 4,
    channel_5 = 5,
    channel_6 = 6,
    channel_7 = 7,
    channel_8 = 8,
};

enum class uhd_output_option
{
    disable_BVC_MultiLink = 0,
    auto_uhd              = 1,
    force_2si             = 2,
    force_square_division = 3,
};

struct configuration
{
    unsigned int                           device_index         = 1;
    bluefish_hardware_output_channel       device_stream        = bluefish_hardware_output_channel::channel_1;
    bool                                   embedded_audio       = true;
    hardware_downstream_keyer_mode         hardware_keyer_value = hardware_downstream_keyer_mode::disable;
    hardware_downstream_keyer_audio_source keyer_audio_source =
        hardware_downstream_keyer_audio_source::VideoOutputChannel;
    unsigned int      watchdog_timeout = 2;
    uhd_output_option uhd_mode         = uhd_output_option::disable_BVC_MultiLink;
};

bool get_videooutput_channel_routing_info_from_streamid(bluefish_hardware_output_channel streamid,
                                                        EEpochRoutingElements&           channelSrcElement,
                                                        EEpochRoutingElements&           sdioutputDstElement)
{
    switch (streamid) {
        case bluefish_hardware_output_channel::channel_1:
            channelSrcElement   = EPOCH_SRC_OUTPUT_MEM_INTERFACE_CH1;
            sdioutputDstElement = EPOCH_DEST_SDI_OUTPUT_1;
            break;
        case bluefish_hardware_output_channel::channel_2:
            channelSrcElement   = EPOCH_SRC_OUTPUT_MEM_INTERFACE_CH2;
            sdioutputDstElement = EPOCH_DEST_SDI_OUTPUT_2;
            break;
        case bluefish_hardware_output_channel::channel_3:
            channelSrcElement   = EPOCH_SRC_OUTPUT_MEM_INTERFACE_CH3;
            sdioutputDstElement = EPOCH_DEST_SDI_OUTPUT_3;
            break;
        case bluefish_hardware_output_channel::channel_4:
            channelSrcElement   = EPOCH_SRC_OUTPUT_MEM_INTERFACE_CH4;
            sdioutputDstElement = EPOCH_DEST_SDI_OUTPUT_4;
            break;
        case bluefish_hardware_output_channel::channel_5:
            channelSrcElement   = EPOCH_SRC_OUTPUT_MEM_INTERFACE_CH5;
            sdioutputDstElement = EPOCH_DEST_SDI_OUTPUT_5;
            break;
        case bluefish_hardware_output_channel::channel_6:
            channelSrcElement   = EPOCH_SRC_OUTPUT_MEM_INTERFACE_CH6;
            sdioutputDstElement = EPOCH_DEST_SDI_OUTPUT_6;
            break;
        case bluefish_hardware_output_channel::channel_7:
            channelSrcElement   = EPOCH_SRC_OUTPUT_MEM_INTERFACE_CH7;
            sdioutputDstElement = EPOCH_DEST_SDI_OUTPUT_7;
            break;
        case bluefish_hardware_output_channel::channel_8:
            channelSrcElement   = EPOCH_SRC_OUTPUT_MEM_INTERFACE_CH8;
            sdioutputDstElement = EPOCH_DEST_SDI_OUTPUT_8;
            break;

        default:
            return false;
    }
    return true;
}

EBlueVideoChannel get_bluesdk_videochannel_from_streamid(bluefish_hardware_output_channel streamid)
{
    /*This function would return the corresponding EBlueVideoChannel from the device output channel*/
    switch (streamid) {
        case bluefish_hardware_output_channel::channel_1:
            return BLUE_VIDEO_OUTPUT_CHANNEL_1;
        case bluefish_hardware_output_channel::channel_2:
            return BLUE_VIDEO_OUTPUT_CHANNEL_2;
        case bluefish_hardware_output_channel::channel_3:
            return BLUE_VIDEO_OUTPUT_CHANNEL_3;
        case bluefish_hardware_output_channel::channel_4:
            return BLUE_VIDEO_OUTPUT_CHANNEL_4;
        case bluefish_hardware_output_channel::channel_5:
            return BLUE_VIDEO_OUTPUT_CHANNEL_5;
        case bluefish_hardware_output_channel::channel_6:
            return BLUE_VIDEO_OUTPUT_CHANNEL_6;
        case bluefish_hardware_output_channel::channel_7:
            return BLUE_VIDEO_OUTPUT_CHANNEL_7;
        case bluefish_hardware_output_channel::channel_8:
            return BLUE_VIDEO_OUTPUT_CHANNEL_8;
        default:
            return BLUE_VIDEO_OUTPUT_CHANNEL_1;
    }
}

struct bluefish_consumer
{
    const int           channel_index_;
    const configuration config_;

    spl::shared_ptr<bvc_wrapper> blue_         = create_blue(config_.device_index);
    spl::shared_ptr<bvc_wrapper> watchdog_bvc_ = create_blue(config_.device_index);

    std::mutex         exception_mutex_;
    std::exception_ptr exception_;

    std::wstring                  model_name_;
    const core::video_format_desc format_desc_;

    std::mutex              buffer_mutex_;
    std::condition_variable buffer_cond_;

    std::atomic<int64_t> scheduled_frames_completed_{0};

    int               field_count_;
    std::atomic<bool> abort_request_{false};

    unsigned int mode_; // ie bf video mode / format
    bool         interlaced_ = false;

    std::array<blue_dma_buffer_ptr, BLUEFISH_MAX_SOFTWARE_BUFFERS> all_frames_;
    tbb::concurrent_bounded_queue<blue_dma_buffer_ptr>             reserved_frames_;
    tbb::concurrent_bounded_queue<blue_dma_buffer_ptr>             live_frames_;

    std::atomic<int64_t> audio_frames_filled_{0};
    blue_dma_buffer_ptr  last_field_buf_ = nullptr;

    std::vector<uint32_t> tmp_audio_buf_;
    unsigned int          tmp_audio_buf_contains_samples = 0;

    std::shared_ptr<std::thread> dma_present_thread_;
    std::shared_ptr<std::thread> hardware_watchdog_thread_;
    std::atomic<bool>            end_hardware_watchdog_thread_;
    unsigned int                 interrupts_to_wait_ = config_.watchdog_timeout;

    spl::shared_ptr<diagnostics::graph> graph_;
    caspar::timer                       tick_timer_;
    caspar::timer                       sync_timer_;

    bluefish_consumer(const bluefish_consumer&)            = delete;
    bluefish_consumer& operator=(const bluefish_consumer&) = delete;

    bluefish_consumer(const configuration& config, const core::video_format_desc& format_desc, int channel_index)
        : channel_index_(channel_index)
        , config_(config)
        , format_desc_(format_desc)
    {
        // OK this is the Guts of it, lets see what we can do to get a compile working, and then some actual
        // functionality eh?
        graph_->set_color("tick-time", diagnostics::color(0.0f, 0.6f, 0.9f));
        graph_->set_color("late-frame", diagnostics::color(0.6f, 0.3f, 0.3f));
        graph_->set_color("dropped-frame", diagnostics::color(0.3f, 0.6f, 0.3f));
        graph_->set_color("flushed-frame", diagnostics::color(0.4f, 0.3f, 0.8f));
        graph_->set_color("buffered-audio", diagnostics::color(0.9f, 0.9f, 0.5f));
        graph_->set_color("buffered-video", diagnostics::color(0.2f, 0.9f, 0.9f));

        graph_->set_text(print());
        diagnostics::register_graph(graph_);

        reserved_frames_.set_capacity(BLUEFISH_MAX_SOFTWARE_BUFFERS);
        live_frames_.set_capacity(BLUEFISH_MAX_SOFTWARE_BUFFERS);

        // get BF video mode
        mode_ = get_bluefish_video_format(format_desc_.format);
        if (mode_ == VID_FMT_EXT_1080I_5000 || mode_ == VID_FMT_EXT_1080I_5994 || mode_ == VID_FMT_EXT_1080I_6000 ||
            mode_ == VID_FMT_EXT_PAL || mode_ == VID_FMT_EXT_NTSC) {
            interlaced_ = true;
        }

        // Specify the video channel
        setup_hardware_output_channel(); // ie stream id

        model_name_ = get_card_desc(*blue_.get(), (int)config_.device_index);

        // disable the video output while we do all the config.
        disable_video_output();

        // check if we need to set Multilink, and configure if required
        setup_multlink();

        // Setting output Video mode
        if (blue_->set_card_property32(VIDEO_MODE_EXT_OUTPUT, mode_))
            CASPAR_THROW_EXCEPTION(caspar_exception() << msg_info(print() + L" Failed to set videomode."));

        // Select Update Mode for output
        if (blue_->set_card_property32(VIDEO_UPDATE_TYPE, UPD_FMT_FRAME))
            CASPAR_THROW_EXCEPTION(caspar_exception() << msg_info(print() + L" Failed to set update type."));

        setup_hardware_output_channel_routing();

        // Select output memory format
        if (blue_->set_card_property32(VIDEO_MEMORY_FORMAT, MEM_FMT_ARGB_PC))
            CASPAR_THROW_EXCEPTION(caspar_exception() << msg_info(print() + L" Failed to set memory format."));

        // Select image orientation
        if (blue_->set_card_property32(VIDEO_IMAGE_ORIENTATION, ImageOrientation_Normal))
            CASPAR_LOG(warning) << print() << L" Failed to set image orientation to normal.";

        // Select data range
        if (blue_->set_card_property32(VIDEO_RGB_DATA_RANGE, CGR_RANGE))
            CASPAR_LOG(warning) << print() << L" Failed to set RGB data range to CGR.";

        // configure audio
        if (!config_.embedded_audio ||
            (config_.hardware_keyer_value == hardware_downstream_keyer_mode::internal &&
             config.keyer_audio_source == hardware_downstream_keyer_audio_source::SDIVideoInput)) {
            if (blue_->set_card_property32(EMBEDEDDED_AUDIO_OUTPUT, 0))
                CASPAR_LOG(warning) << TEXT("BLUECARD ERROR: Failed to disable embedded audio.");
            CASPAR_LOG(info) << print() << TEXT(" Disabled embedded-audio.");
        } else {
            ULONG audio_value = blue_emb_audio_enable | blue_emb_audio_group1_enable;
            if (format_desc_.audio_channels > 4)
                audio_value |= blue_emb_audio_group2_enable;

            if (format_desc_.audio_channels > 8)
                audio_value |= blue_emb_audio_group3_enable;

            if (format_desc_.audio_channels > 12)
                audio_value |= blue_emb_audio_group4_enable;

            if (blue_->set_card_property32(EMBEDEDDED_AUDIO_OUTPUT, audio_value))
                CASPAR_LOG(warning) << print() << TEXT(" Failed to enable embedded audio.");
            CASPAR_LOG(info) << print() << TEXT(" Enabled embedded-audio.");
        }

        if (blue_->set_card_property32(VIDEO_OUTPUT_ENGINE, VIDEO_ENGINE_PLAYBACK))
            CASPAR_LOG(warning) << print() << TEXT(" Failed to set video engine.");

        if (is_epoch_card(*blue_) && is_epoch_neutron_1i2o_card(*blue_))
            setup_hardware_downstream_keyer(config.hardware_keyer_value, config.keyer_audio_source);

        // ok here we create a bunch of Bluefish buffers, that contain video and encoded hanc....
        // this is the software Q. / software buffers
        int n = 0;
        boost::range::generate(
            all_frames_, [&] { return std::make_shared<blue_dma_buffer>(static_cast<int>(format_desc_.size), n++); });

        for (size_t i = 0; i < all_frames_.size(); i++)
            reserved_frames_.push(all_frames_[i]);

        tmp_audio_buf_.reserve(SIZE_TEMP_AUDIO_BUFFER);

        // start the thread if required.
        if (dma_present_thread_ == nullptr) {
            dma_present_thread_ = std::make_shared<std::thread>([this] { dma_present_thread_actual(); });
#if defined(_WIN32)
            HANDLE handle = (HANDLE)dma_present_thread_->native_handle();
            SetThreadPriority(handle, THREAD_PRIORITY_HIGHEST);
#endif
        }

        configure_watchdog();
        enable_video_output();
    }

    ~bluefish_consumer()
    {
        try {
            abort_request_ = true;
            buffer_cond_.notify_all();

            if (!end_hardware_watchdog_thread_)
                disable_watchdog();

            disable_video_output();
            watchdog_bvc_->detach();
            blue_->detach();

            if (dma_present_thread_)
                dma_present_thread_->join();

            if (hardware_watchdog_thread_)
                hardware_watchdog_thread_->join();
        } catch (...) {
            CASPAR_LOG_CURRENT_EXCEPTION();
        }
    }

    void watchdog_thread_actual()
    {
        watchdog_bvc_->attach(config_.device_index);
        EBlueVideoChannel out_vid_channel = get_bluesdk_videochannel_from_streamid(config_.device_stream);
        watchdog_bvc_->set_card_property32(DEFAULT_VIDEO_OUTPUT_CHANNEL, out_vid_channel);

        unsigned long fc = 0;
        unsigned int  blue_prop =
            EPOCH_WATCHDOG_TIMER_SET_MACRO(enum_blue_app_watchdog_timer_keepalive, config_.watchdog_timeout);

        while (!end_hardware_watchdog_thread_) {
            watchdog_bvc_->wait_video_output_sync(UPD_FMT_FIELD, &fc);
            watchdog_bvc_->set_card_property32(EPOCH_APP_WATCHDOG_TIMER, blue_prop);
        }

        disable_watchdog();
        watchdog_bvc_->detach();
    }

    void configure_watchdog()
    {
        // First test if we even want to enable the watchdog, only on Ch 1 and if user has not explicitly set count to
        // 0, and only if card has at least 1  input sdi, else dont do anything
        unsigned int val = 0;
        blue_->get_card_property32(CARD_FEATURE_STREAM_INFO, &val);
        unsigned int num_input_streams = CARD_FEATURE_GET_SDI_INPUT_STREAM_COUNT(val);
        if ((interrupts_to_wait_ != 0u) && config_.device_stream == bluefish_hardware_output_channel::channel_1 &&
            (num_input_streams != 0u)) {
            // check if it is already running
            unsigned int blue_prop =
                EPOCH_WATCHDOG_TIMER_SET_MACRO(enum_blue_app_watchdog_get_timer_activated_status, 0);
            blue_->get_card_property32(EPOCH_APP_WATCHDOG_TIMER, &blue_prop);
            if (EPOCH_WATCHDOG_TIMER_GET_VALUE_MACRO(blue_prop)) {
                // watchdog timer is running already, switch it off.
                blue_prop = EPOCH_WATCHDOG_TIMER_SET_MACRO(enum_blue_app_watchdog_timer_start_stop, (unsigned int)0);
                blue_->set_card_property32(EPOCH_APP_WATCHDOG_TIMER, blue_prop);
            }

            // Setting up the watchdog properties
            unsigned int watchdog_timer_gpo_port = 1; // GPO port to use: 0 = none, 1 = port A, 2 = port B
            blue_prop =
                EPOCH_WATCHDOG_TIMER_SET_MACRO(enum_blue_app_watchdog_enable_gpo_on_active, watchdog_timer_gpo_port);
            blue_->set_card_property32(EPOCH_APP_WATCHDOG_TIMER, blue_prop);

            if (interrupts_to_wait_ == 1) // using too low a value can cause instability on the watchdog so always make
                                          // sure we use a value of 2 or more...
                interrupts_to_wait_++;

            blue_prop = EPOCH_WATCHDOG_TIMER_SET_MACRO(enum_blue_app_watchdog_timer_start_stop, interrupts_to_wait_);
            blue_->set_card_property32(EPOCH_APP_WATCHDOG_TIMER, blue_prop);

            // start the thread if required.
            if (hardware_watchdog_thread_ == nullptr) {
                end_hardware_watchdog_thread_ = false;
                hardware_watchdog_thread_     = std::make_shared<std::thread>([this] { watchdog_thread_actual(); });
            }
        }
    }

    void disable_watchdog()
    {
        end_hardware_watchdog_thread_ = true;
        unsigned int stop_value       = 0;
        unsigned int blue_prop = EPOCH_WATCHDOG_TIMER_SET_MACRO(enum_blue_app_watchdog_timer_start_stop, stop_value);
        watchdog_bvc_->get_card_property32(EPOCH_APP_WATCHDOG_TIMER, &blue_prop);
    }

    void setup_hardware_output_channel()
    {
        // This function would be used to setup the logic video channel in the bluefish hardware
        EBlueVideoChannel out_vid_channel = get_bluesdk_videochannel_from_streamid(config_.device_stream);
        if (is_epoch_card(*blue_) || is_kronos_card(*blue_)) {
            if (out_vid_channel != BLUE_VIDEOCHANNEL_INVALID) {
                if (blue_->set_card_property32(DEFAULT_VIDEO_OUTPUT_CHANNEL, out_vid_channel))
                    CASPAR_THROW_EXCEPTION(caspar_exception() << msg_info(" Failed to set video stream."));

                blue_->video_playback_stop(0, 0);
            }
        }
    }

    void setup_multlink()
    {
        // We only want to enable multi-link in specific scenarios, so lets check all those conditions first
        EBlueVideoChannel out_vid_channel = get_bluesdk_videochannel_from_streamid(config_.device_stream);
        if (is_kronos_card(*blue_) &&
            (out_vid_channel == BLUE_VIDEO_OUTPUT_CHANNEL_1 || out_vid_channel == BLUE_VIDEO_OUTPUT_CHANNEL_5) &&
            (config_.uhd_mode != uhd_output_option::disable_BVC_MultiLink) && (format_desc_.width > 2048)) {
            unsigned int val = mode_;
            blue_->set_multilink(config_.device_index, out_vid_channel);

            // Now lest test to see if our MultiLink enable instance can support the video Mode in Q.
            blue_->get_card_property32(IS_VIDEO_MODE_EXT_SUPPORTED_OUTPUT, &val);
            if (val)
                return;
            else {
                blue_->set_multilink(0, -1);
                CASPAR_THROW_EXCEPTION(caspar_exception() << msg_info(" Failed to set video stream."));
            }
        }
    }

    void setup_hardware_output_channel_routing()
    {
        // This function would be used to setup the dual link and any other routing that would be required .
        if (is_epoch_card(*blue_) || is_kronos_card(*blue_)) {
            EBlueVideoChannel blue_video_output_channel = get_bluesdk_videochannel_from_streamid(config_.device_stream);
            EEpochRoutingElements src_element           = (EEpochRoutingElements)0;
            EEpochRoutingElements dst_element           = (EEpochRoutingElements)0;
            get_videooutput_channel_routing_info_from_streamid(config_.device_stream, src_element, dst_element);
            bool duallink_4224_enabled = false;

            if ((config_.device_stream == bluefish_hardware_output_channel::channel_1 ||
                 config_.device_stream == bluefish_hardware_output_channel::channel_3) &&
                    config_.hardware_keyer_value == hardware_downstream_keyer_mode::external ||
                config_.hardware_keyer_value == hardware_downstream_keyer_mode::internal) {
                duallink_4224_enabled = true;
            }

            // Enable/Disable dual link output
            if (blue_->set_card_property32(VIDEO_DUAL_LINK_OUTPUT, duallink_4224_enabled))
                CASPAR_THROW_EXCEPTION(caspar_exception()
                                       << msg_info(print() + L" Failed to enable/disable dual link."));

            if (!duallink_4224_enabled) {
                if (blue_->set_card_property32(VIDEO_DUAL_LINK_OUTPUT_SIGNAL_FORMAT_TYPE,
                                               Signal_FormatType_Independent_422))
                    CASPAR_THROW_EXCEPTION(caspar_exception()
                                           << msg_info(print() + L" Failed to set dual link format type to 4:2:2."));

                //   ULONG routingValue = EPOCH_SET_ROUTING(src_element, dst_element, BLUE_CONNECTOR_PROP_SINGLE_LINK);
                //    if (blue_->set_card_property32(MR2_ROUTING, routingValue))
                //        CASPAR_THROW_EXCEPTION(caspar_exception() << msg_info(" Failed to MR 2 routing."));

                // If single link 422, but on second channel AND on Neutron we need to set Genlock to Aux.
                if (is_epoch_neutron_1i2o_card(*blue_)) {
                    if (blue_video_output_channel == BLUE_VIDEO_OUTPUT_CHANNEL_B) {
                        ULONG genlock_source = BlueGenlockAux;
                        if (blue_->set_card_property32(VIDEO_GENLOCK_SIGNAL, genlock_source))
                            CASPAR_THROW_EXCEPTION(caspar_exception()
                                                   << msg_info("Failed to set GenLock to Aux Input."));
                    }
                }
                if (is_epoch_neutron_3o_card(*blue_)) {
                    if (blue_video_output_channel == BLUE_VIDEO_OUTPUT_CHANNEL_C) {
                        ULONG genlock_source = BlueGenlockAux;
                        if (blue_->set_card_property32(VIDEO_GENLOCK_SIGNAL, genlock_source))
                            CASPAR_THROW_EXCEPTION(caspar_exception()
                                                   << msg_info("Failed to set GenLock to Aux Input."));
                    } else if (blue_video_output_channel == BLUE_VIDEO_OUTPUT_CHANNEL_B) {
                        ULONG routing_value = EPOCH_SET_ROUTING(EPOCH_SRC_OUTPUT_MEM_INTERFACE_CHB,
                                                                EPOCH_DEST_SDI_OUTPUT_B,
                                                                BLUE_CONNECTOR_PROP_DUALLINK_LINK_1);
                        if (blue_->set_card_property32(MR2_ROUTING, routing_value))
                            CASPAR_THROW_EXCEPTION(caspar_exception() << msg_info("Failed to MR 2 routing."));
                    }
                }
            } else // dual Link IS enabled, ie. 4224 Fill and Key
            {
                if (blue_->set_card_property32(VIDEO_DUAL_LINK_OUTPUT_SIGNAL_FORMAT_TYPE, Signal_FormatType_4224))
                    CASPAR_THROW_EXCEPTION(caspar_exception()
                                           << msg_info(print() + L" Failed to set dual link format type to 4:2:2:4."));

                if (blue_video_output_channel == BLUE_VIDEO_OUTPUT_CHANNEL_1) {
                    ULONG routing_value = EPOCH_SET_ROUTING(EPOCH_SRC_OUTPUT_MEM_INTERFACE_CHA,
                                                            EPOCH_DEST_SDI_OUTPUT_A,
                                                            BLUE_CONNECTOR_PROP_DUALLINK_LINK_1);
                    if (blue_->set_card_property32(MR2_ROUTING, routing_value))
                        CASPAR_THROW_EXCEPTION(caspar_exception() << msg_info("Failed to MR 2 routing."));

                    routing_value = EPOCH_SET_ROUTING(EPOCH_SRC_OUTPUT_MEM_INTERFACE_CHA,
                                                      EPOCH_DEST_SDI_OUTPUT_B,
                                                      BLUE_CONNECTOR_PROP_DUALLINK_LINK_2);
                    if (blue_->set_card_property32(MR2_ROUTING, routing_value))
                        CASPAR_THROW_EXCEPTION(caspar_exception() << msg_info("Failed to MR 2 routing."));

                    if (is_epoch_neutron_1i2o_card(*blue_)) // Neutron cards require setting the Genlock connector to
                                                            // Aux to enable them to do Dual-Link
                    {
                        ULONG genLockSource = BlueGenlockAux;
                        if (blue_->set_card_property32(VIDEO_GENLOCK_SIGNAL, genLockSource))
                            CASPAR_THROW_EXCEPTION(caspar_exception()
                                                   << msg_info("Failed to set GenLock to Aux Input."));
                    } else if (is_epoch_neutron_3o_card(*blue_)) {
                        if (blue_video_output_channel == BLUE_VIDEO_OUTPUT_CHANNEL_C) {
                            ULONG genLockSource = BlueGenlockAux;
                            if (blue_->set_card_property32(VIDEO_GENLOCK_SIGNAL, genLockSource))
                                CASPAR_THROW_EXCEPTION(caspar_exception()
                                                       << msg_info("Failed to set GenLock to Aux Input."));
                        }
                    }
                } else {
                    // using channel C for 4224 on other configurations requires explicit routing
                    if (blue_video_output_channel == BLUE_VIDEO_OUTPUT_CHANNEL_C) {
                        ULONG routingValue = EPOCH_SET_ROUTING(EPOCH_SRC_OUTPUT_MEM_INTERFACE_CHC,
                                                               EPOCH_DEST_SDI_OUTPUT_C,
                                                               BLUE_CONNECTOR_PROP_DUALLINK_LINK_1);
                        if (blue_->set_card_property32(MR2_ROUTING, routingValue))
                            CASPAR_THROW_EXCEPTION(caspar_exception() << msg_info("Failed to MR 2 routing."));
                        routingValue = EPOCH_SET_ROUTING(EPOCH_SRC_OUTPUT_MEM_INTERFACE_CHC,
                                                         EPOCH_DEST_SDI_OUTPUT_D,
                                                         BLUE_CONNECTOR_PROP_DUALLINK_LINK_2);
                        if (blue_->set_card_property32(MR2_ROUTING, routingValue))
                            CASPAR_THROW_EXCEPTION(caspar_exception() << msg_info("Failed to MR 2 routing."));
                    }
                }
            }
        }
    }

    void setup_hardware_downstream_keyer(hardware_downstream_keyer_mode         keyer,
                                         hardware_downstream_keyer_audio_source audio_source)
    {
        unsigned int keyer_control_value = 0;
        if (keyer == hardware_downstream_keyer_mode::disable || keyer == hardware_downstream_keyer_mode::external) {
            keyer_control_value = VIDEO_ONBOARD_KEYER_SET_STATUS_DISABLED(keyer_control_value);
            keyer_control_value = VIDEO_ONBOARD_KEYER_SET_STATUS_DISABLE_OVER_BLACK(keyer_control_value);
        } else if (keyer == hardware_downstream_keyer_mode::internal) {
            unsigned int input_video_mode = 0;
            if (blue_->get_card_property32(VIDEO_MODE_EXT_INPUT, &input_video_mode))
                CASPAR_THROW_EXCEPTION(caspar_exception() << msg_info(" Failed to get invalid video mode flag"));

            // The bluefish HW keyer is NOT going to pre-multiply the RGB with the A.
            keyer_control_value = VIDEO_ONBOARD_KEYER_SET_STATUS_DATA_IS_PREMULTIPLIED(keyer_control_value);

            keyer_control_value = VIDEO_ONBOARD_KEYER_SET_STATUS_ENABLED(keyer_control_value);
            if (blue_->get_card_property32(VIDEO_INPUT_SIGNAL_VIDEO_MODE, &input_video_mode))
                CASPAR_THROW_EXCEPTION(caspar_exception() << msg_info(" Failed to get video input signal mode"));

            if (input_video_mode == VID_FMT_EXT_INVALID)
                keyer_control_value = VIDEO_ONBOARD_KEYER_SET_STATUS_ENABLE_OVER_BLACK(keyer_control_value);
            else
                keyer_control_value = VIDEO_ONBOARD_KEYER_SET_STATUS_DISABLE_OVER_BLACK(keyer_control_value);

            // lock to input
            if (blue_->set_card_property32(
                    VIDEO_GENLOCK_SIGNAL,
                    BlueSDI_A_BNC)) // todo: will need to adjust when we support keyer on all channels...
                CASPAR_THROW_EXCEPTION(caspar_exception()
                                       << msg_info(" Failed to set the genlock to the input for the HW keyer"));
        }

        if (audio_source == hardware_downstream_keyer_audio_source::SDIVideoInput &&
            keyer == hardware_downstream_keyer_mode::internal)
            keyer_control_value = VIDEO_ONBOARD_KEYER_SET_STATUS_USE_INPUT_ANCILLARY(keyer_control_value);
        else if (audio_source == hardware_downstream_keyer_audio_source::VideoOutputChannel)
            keyer_control_value = VIDEO_ONBOARD_KEYER_SET_STATUS_USE_OUTPUT_ANCILLARY(keyer_control_value);

        if (blue_->set_card_property32(VIDEO_ONBOARD_KEYER, keyer_control_value))
            CASPAR_LOG(error) << print() << TEXT(" Failed to set keyer control.");
    }

    void enable_video_output()
    {
        if (config_.device_stream == bluefish_hardware_output_channel::channel_1)
            blue_->set_card_property32(BYPASS_RELAY_A_ENABLE, 0);
        else if (config_.device_stream == bluefish_hardware_output_channel::channel_2)
            blue_->set_card_property32(BYPASS_RELAY_B_ENABLE, 0);

        if (blue_->set_card_property32(VIDEO_BLACKGENERATOR, 0))
            CASPAR_LOG(error) << print() << TEXT(" Failed to disable video output.");
    }

    void disable_video_output()
    {
        blue_->video_playback_stop(0, 0);
        blue_->set_card_property32(VIDEO_DUAL_LINK_OUTPUT, 0);

        if (blue_->set_card_property32(VIDEO_BLACKGENERATOR, 1))
            CASPAR_LOG(error) << print() << TEXT(" Failed to disable video output.");
        if (blue_->set_card_property32(EMBEDEDDED_AUDIO_OUTPUT, 0))
            CASPAR_LOG(error) << print() << TEXT(" Failed to disable audio output.");
    }

    bool send(core::video_field field, core::const_frame frame)
    {
        // TODO - field alignment
        {
            std::lock_guard<std::mutex> lock(exception_mutex_);
            if (exception_ != nullptr) {
                std::rethrow_exception(exception_);
            }
        }

        if (!frame) {
            return !abort_request_;
        }

        try {
            std::unique_lock<std::mutex> lock(buffer_mutex_);
            copy_frame(frame);
            graph_->set_value("tick-time", static_cast<float>(tick_timer_.elapsed() * format_desc_.fps * 0.5));
            tick_timer_.restart();
        } catch (...) {
            CASPAR_LOG_CURRENT_EXCEPTION();
        }

        return !abort_request_;
    }

    void dma_present_thread_actual()
    {
        bvc_wrapper wait_b;
        wait_b.attach(config_.device_index);
        EBlueVideoChannel out_vid_channel = get_bluesdk_videochannel_from_streamid(config_.device_stream);
        wait_b.set_card_property32(DEFAULT_VIDEO_OUTPUT_CHANNEL, out_vid_channel);
        int           frames_to_buffer = 1;
        unsigned long buffer_id        = 0;
        unsigned long underrun         = 0;

        while (!abort_request_) {
            blue_dma_buffer_ptr buf = nullptr;
            if (live_frames_.try_pop(buf) && (blue_->video_playback_allocate(&buffer_id, &underrun) == 0)) {
                // Send and display
                if (config_.embedded_audio) {
                    // Do video first, then do hanc DMA...
                    blue_->system_buffer_write(const_cast<uint8_t*>(buf->image_data()),
                                               static_cast<unsigned long>(buf->image_size()),
                                               BlueImage_HANC_DMABuffer(buffer_id, BLUE_DATA_IMAGE),
                                               0);

                    blue_->system_buffer_write(buf->hanc_data(),
                                               static_cast<unsigned long>(buf->hanc_size()),
                                               BlueImage_HANC_DMABuffer(buffer_id, BLUE_DATA_HANC),
                                               0);

                    if (blue_->video_playback_present(BlueBuffer_Image_HANC(buffer_id), 1, 0, 0))
                        CASPAR_LOG(warning) << print() << TEXT(" video_playback_present failed.");
                } else {
                    blue_->system_buffer_write(const_cast<uint8_t*>(buf->image_data()),
                                               static_cast<unsigned long>(buf->image_size()),
                                               BlueImage_DMABuffer(buffer_id, BLUE_DATA_IMAGE),
                                               0);

                    if (blue_->video_playback_present(BlueBuffer_Image(buffer_id), 1, 0, 0))
                        CASPAR_LOG(warning) << print() << TEXT(" video_playback_present failed.");
                }

                ++scheduled_frames_completed_;
                reserved_frames_.push(buf);
            }

            {
                // Sync and Update timer
                unsigned long n_field = 0;
                wait_b.wait_video_output_sync(UPD_FMT_FRAME, &n_field);
                graph_->set_value("sync-time", sync_timer_.elapsed() * format_desc_.fps * 0.5);
                sync_timer_.restart();
            }

            if (frames_to_buffer > 0) {
                frames_to_buffer--;
                if (frames_to_buffer == 0) {
                    if (blue_->video_playback_start(0, 0))
                        CASPAR_LOG(warning) << print() << TEXT("Error video playback start failed");
                }
            }
        }
        wait_b.detach();
        blue_->video_playback_stop(0, 0);
    }

    void copy_frame(core::const_frame frame)
    {
        int audio_samples_for_next_frame =
            blue_->get_num_audio_samples_for_frame(mode_, static_cast<unsigned int>(audio_frames_filled_));

        if (interlaced_) {
            if (!last_field_buf_) // field 1
            {
                if (reserved_frames_.try_pop(last_field_buf_)) {
                    // copy video data into holding buf
                    void* dest = last_field_buf_->image_data();
                    if (frame.image_data(0).size()) {
                        std::memcpy(dest, frame.image_data(0).begin(), frame.image_data(0).size());
                    } else
                        std::memset(dest, 0, last_field_buf_->image_size());

                    // now copy Some of the Audio bytes that we need
                    if (config_.embedded_audio) {
                        auto audio_size = frame.audio_data().size() * 4;
                        if (audio_size) {
                            tmp_audio_buf_.insert(
                                tmp_audio_buf_.end(), frame.audio_data().begin(), frame.audio_data().end());
                        }
                    }
                }
            } else // field 2
            {
                // we have already done the video... just grab the last bit of audio, encode and push to Q.
                if (config_.embedded_audio) {
                    auto audio_size = frame.audio_data().size() * 4;
                    if (audio_size) {
                        tmp_audio_buf_.insert(
                            tmp_audio_buf_.end(), frame.audio_data().begin(), frame.audio_data().end());

                        encode_hanc(reinterpret_cast<BLUE_U32*>(last_field_buf_->hanc_data()),
                                    reinterpret_cast<void*>(tmp_audio_buf_.data()),
                                    audio_samples_for_next_frame,
                                    static_cast<int>(format_desc_.audio_channels));
                        ++audio_frames_filled_;
                    }
                }
                // push to in use Q.
                live_frames_.push(last_field_buf_);
                last_field_buf_ = nullptr;
                tmp_audio_buf_.clear();
            }
        } else {
            blue_dma_buffer_ptr buf = nullptr;
            // Copy to local buffers
            if (reserved_frames_.try_pop(buf)) {
                void* dest = buf->image_data();
                if (frame.image_data(0).size()) {
                    if (config_.uhd_mode == uhd_output_option::force_2si) {
                        // Do the Square Division top 2si conversion here.
                        blue_->convert_sq_to_2si(
                            (int)frame.width(), (int)frame.height(), (void*)frame.image_data(0).begin(), dest);
                    } else
                        std::memcpy(dest, frame.image_data(0).begin(), frame.image_data(0).size());
                } else
                    std::memset(dest, 0, buf->image_size());

                // encode and copy hanc data
                if (config_.embedded_audio) {
                    if (frame.audio_data().size()) {
                        encode_hanc(reinterpret_cast<BLUE_U32*>(buf->hanc_data()),
                                    (void*)frame.audio_data().data(),
                                    audio_samples_for_next_frame,
                                    static_cast<int>(format_desc_.audio_channels));
                        ++audio_frames_filled_;
                    }
                }
                // push to in use Q.
                live_frames_.push(buf);
            }
        }

        // Sync
        unsigned long n_field = 0;
        if (interlaced_)
            blue_->wait_video_output_sync(UPD_FMT_FIELD, &n_field);
        else
            blue_->wait_video_output_sync(UPD_FMT_FRAME, &n_field);
        graph_->set_value("sync-time", sync_timer_.elapsed() * format_desc_.fps * 0.5);
        sync_timer_.restart();
    }

    void encode_hanc(BLUE_U32* hanc_data, void* audio_data, int audio_samples, int audio_nchannels)
    {
        const auto sample_type    = AUDIO_CHANNEL_LITTLEENDIAN;
        auto       emb_audio_flag = blue_emb_audio_enable | blue_emb_audio_group1_enable;
        if (audio_nchannels > 4)
            emb_audio_flag |= blue_emb_audio_group2_enable;
        if (audio_nchannels > 8)
            emb_audio_flag |= blue_emb_audio_group3_enable;
        if (audio_nchannels > 12)
            emb_audio_flag |= blue_emb_audio_group4_enable;

        hanc_stream_info_struct hanc_stream_info;
        std::memset(&hanc_stream_info, 0, sizeof(hanc_stream_info));

        hanc_stream_info.AudioDBNArray[0] = -1;
        hanc_stream_info.AudioDBNArray[1] = -1;
        hanc_stream_info.AudioDBNArray[2] = -1;
        hanc_stream_info.AudioDBNArray[3] = -1;
        hanc_stream_info.hanc_data_ptr    = hanc_data;
        hanc_stream_info.video_mode       = get_bluefish_video_format(format_desc_.format);

        int card_type = CRD_INVALID;
        blue_->query_card_type(&card_type, config_.device_index);
        blue_->encode_hanc_frame(
            card_type, &hanc_stream_info, audio_data, audio_nchannels, audio_samples, sample_type, emb_audio_flag);
    }

    std::wstring print() const
    {
        return model_name_ + L" [" + std::to_wstring(channel_index_) + L"-" + std::to_wstring(config_.device_index) +
               L"Stream: " + std::to_wstring(static_cast<unsigned int>(config_.device_stream)) + L"|" +
               format_desc_.name + L"]";
    }

    int64_t presentation_delay_millis() const { return 0; }
};

struct bluefish_consumer_proxy : public core::frame_consumer
{
    const configuration                config_;
    std::unique_ptr<bluefish_consumer> consumer_;
    core::video_format_desc            format_desc_;
    executor                           executor_;

  public:
    explicit bluefish_consumer_proxy(const configuration& config)
        : config_(config)
        , executor_(L"bluefish_consumer[" + std::to_wstring(config.device_index) + L"]")
    {
    }

    ~bluefish_consumer_proxy()
    {
        executor_.invoke([=, this] { consumer_.reset(); });
    }

    // frame_consumer
    void initialize(const core::video_format_desc& format_desc,
                    const core::channel_info&      channel_info,
                    int                            port_index) override
    {
        format_desc_ = format_desc;
        executor_.invoke([=, this] {
            consumer_.reset();
            consumer_.reset(new bluefish_consumer(config_, format_desc, channel_info.index));
        });
    }

    std::future<bool> send(core::video_field field, core::const_frame frame) override
    {
        return executor_.begin_invoke([=, this] { return consumer_->send(field, frame); });
    }

    std::wstring print() const override
    {
        auto id_str = consumer_id().empty() ? L"" : L"|id=" + consumer_id();
        return consumer_ ? consumer_->print() + id_str : L"[bluefish_consumer" + id_str + L"]";
    }

    std::wstring name() const override { return L"bluefish"; }

    int index() const override { return 400 + config_.device_index; }

    bool has_synchronization_clock() const override { return true; }

    core::monitor::state state() const override
    {
        core::monitor::state state;
        state["bluefish/index"]          = config_.device_index;
        state["bluefish/stream"]         = static_cast<unsigned int>(config_.device_stream);
        state["bluefish/embedded_audio"] = config_.embedded_audio;
        if (!consumer_id().empty())
            state["bluefish/consumer_id"] = consumer_id();
        return state;
    }
};

spl::shared_ptr<core::frame_consumer> create_consumer(const std::vector<std::wstring>&     params,
                                                      const core::video_format_repository& format_repository,
                                                      const std::vector<spl::shared_ptr<core::video_channel>>& channels,
                                                      const core::channel_info& channel_info)
{
    if (params.size() < 1 || !boost::iequals(params.at(0), L"BLUEFISH")) {
        return core::frame_consumer::empty();
    }

    if (channel_info.depth != common::bit_depth::bit8)
        CASPAR_THROW_EXCEPTION(caspar_exception() << msg_info("Bluefish consumer only supports 8-bit color depth."));

    configuration config;

    // const auto device_index       = params.size() > 1 ? std::stoi(params.at(1)) : 1;
    // const auto device_stream      = contains_param(L"SDI-STREAM", params);
    // const auto embedded_audio     = contains_param(L"EMBEDDED_AUDIO", params);
    // const auto keyer_option       = contains_param(L"KEYER", params);
    // const auto keyer_audio_option = contains_param(L"INTERNAL-KEYER-AUDIO-SOURCE", params);

    config.device_stream = bluefish_hardware_output_channel::channel_1;
    if (contains_param(L"1", params))
        config.device_stream = bluefish_hardware_output_channel::channel_1;
    else if (contains_param(L"2", params))
        config.device_stream = bluefish_hardware_output_channel::channel_2;
    else if (contains_param(L"3", params))
        config.device_stream = bluefish_hardware_output_channel::channel_3;
    else if (contains_param(L"4", params))
        config.device_stream = bluefish_hardware_output_channel::channel_4;
    else if (contains_param(L"5", params))
        config.device_stream = bluefish_hardware_output_channel::channel_5;
    else if (contains_param(L"6", params))
        config.device_stream = bluefish_hardware_output_channel::channel_6;
    else if (contains_param(L"7", params))
        config.device_stream = bluefish_hardware_output_channel::channel_7;
    else if (contains_param(L"8", params))
        config.device_stream = bluefish_hardware_output_channel::channel_8;

    config.hardware_keyer_value = hardware_downstream_keyer_mode::disable;
    if (contains_param(L"DISABLED", params))
        config.hardware_keyer_value = hardware_downstream_keyer_mode::disable;
    else if (contains_param(L"EXTERNAL", params))
        config.hardware_keyer_value = hardware_downstream_keyer_mode::external;
    else if (contains_param(L"INTERNAL", params))
        config.hardware_keyer_value = hardware_downstream_keyer_mode::internal;

    config.keyer_audio_source = hardware_downstream_keyer_audio_source::SDIVideoInput;
    if (contains_param(L"SDIVIDEOINPUT", params))
        config.keyer_audio_source = hardware_downstream_keyer_audio_source::SDIVideoInput;
    else if (contains_param(L"VIDEOOUTPUTCHANNEL", params))
        config.keyer_audio_source = hardware_downstream_keyer_audio_source::VideoOutputChannel;

    config.embedded_audio   = contains_param(L"EMBEDDED_AUDIO", params);
    config.watchdog_timeout = 2;

    return spl::make_shared<bluefish_consumer_proxy>(config);
}

spl::shared_ptr<core::frame_consumer>
create_preconfigured_consumer(const boost::property_tree::wptree&                      ptree,
                              const core::video_format_repository&                     format_repository,
                              const std::vector<spl::shared_ptr<core::video_channel>>& channels,
                              const core::channel_info&                                channel_info)
{
    configuration config;
    auto          device_index = ptree.get(L"device", 1);
    config.device_index        = device_index;

    if (channel_info.depth != common::bit_depth::bit8)
        CASPAR_THROW_EXCEPTION(caspar_exception() << msg_info("Bluefish consumer only supports 8-bit color depth."));

    auto device_stream = ptree.get(L"sdi-stream", L"1");
    if (device_stream == L"1")
        config.device_stream = bluefish_hardware_output_channel::channel_1;
    else if (device_stream == L"2")
        config.device_stream = bluefish_hardware_output_channel::channel_2;
    else if (device_stream == L"3")
        config.device_stream = bluefish_hardware_output_channel::channel_3;
    else if (device_stream == L"4")
        config.device_stream = bluefish_hardware_output_channel::channel_4;
    else if (device_stream == L"5")
        config.device_stream = bluefish_hardware_output_channel::channel_5;
    else if (device_stream == L"6")
        config.device_stream = bluefish_hardware_output_channel::channel_6;
    else if (device_stream == L"7")
        config.device_stream = bluefish_hardware_output_channel::channel_7;
    else if (device_stream == L"8")
        config.device_stream = bluefish_hardware_output_channel::channel_8;

    auto embedded_audio   = ptree.get(L"embedded-audio", false);
    config.embedded_audio = embedded_audio;

    auto hardware_keyer_value = ptree.get(L"keyer", L"disabled");
    if (hardware_keyer_value == L"disabled")
        config.hardware_keyer_value = hardware_downstream_keyer_mode::disable;
    else if (hardware_keyer_value == L"external")
        config.hardware_keyer_value = hardware_downstream_keyer_mode::external;
    else if (hardware_keyer_value == L"internal")
        config.hardware_keyer_value = hardware_downstream_keyer_mode::internal;

    auto keyer_audio_source_value = ptree.get(L"internal-keyer-audio-source", L"videooutputchannel");
    if (keyer_audio_source_value == L"videooutputchannel")
        config.keyer_audio_source = hardware_downstream_keyer_audio_source::VideoOutputChannel;
    else if (keyer_audio_source_value == L"sdivideoinput")
        config.keyer_audio_source = hardware_downstream_keyer_audio_source::SDIVideoInput;

    auto watchdog_timeout   = ptree.get(L"watchdog", 2);
    config.watchdog_timeout = watchdog_timeout;

    auto uhd_mode   = ptree.get(L"uhd-mode", 0);
    config.uhd_mode = uhd_output_option::disable_BVC_MultiLink;
    if (uhd_mode == 1)
        config.uhd_mode = uhd_output_option::auto_uhd;
    else if (uhd_mode == 2)
        config.uhd_mode = uhd_output_option::force_2si;
    else if (uhd_mode == 3)
        config.uhd_mode = uhd_output_option::force_square_division;

    return spl::make_shared<bluefish_consumer_proxy>(config);
}

}} // namespace caspar::bluefish
