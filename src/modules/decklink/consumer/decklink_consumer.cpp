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
 * Author: Julian Waller, julian@superfly.tv
 */

#include "../StdAfx.h"

#include "common/os/thread.h"
#include "config.h"
#include "decklink_consumer.h"
#include "format_strategy.h"
#include "monitor.h"
#include "vanc.h"

#include "../util/util.h"

#include <core/consumer/channel_info.h>
#include <core/consumer/frame_consumer.h>
#include <core/frame/frame.h>
#include <core/frame/pixel_format.h>
#include <core/video_format.h>

#include <common/diagnostics/graph.h>
#include <common/except.h>
#include <common/executor.h>
#include <common/timer.h>

#include <tbb/parallel_for.h>

#include <boost/circular_buffer.hpp>

#include <atomic>
#include <common/prec_timer.h>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <thread>
#include <utility>

namespace caspar { namespace decklink {

template <typename Configuration>
void set_latency(const com_iface_ptr<Configuration>& config,
                 configuration::latency_t            latency,
                 const std::wstring&                 print)
{
    if (latency == configuration::latency_t::low_latency) {
        config->SetFlag(bmdDeckLinkConfigLowLatencyVideoOutput, true);
        CASPAR_LOG(info) << print << L" Enabled low-latency mode.";
    } else if (latency == configuration::latency_t::normal_latency) {
        config->SetFlag(bmdDeckLinkConfigLowLatencyVideoOutput, false);
        CASPAR_LOG(info) << print << L" Disabled low-latency mode.";
    }
}

com_ptr<IDeckLinkDisplayMode> get_display_mode(const com_iface_ptr<IDeckLinkOutput>& device,
                                               core::video_format                    fmt,
                                               BMDPixelFormat                        pix_fmt,
                                               BMDSupportedVideoModeFlags            flag)
{
    auto format = get_decklink_video_format(fmt);

    IDeckLinkDisplayMode*         m = nullptr;
    IDeckLinkDisplayModeIterator* iter;
    if (SUCCEEDED(device->GetDisplayModeIterator(&iter))) {
        auto iterator = wrap_raw<com_ptr>(iter, true);
        while (SUCCEEDED(iterator->Next(&m)) && m != nullptr && m->GetDisplayMode() != format) {
            m->Release();
        }
    }

    if (!m)
        CASPAR_THROW_EXCEPTION(user_error()
                               << msg_info("Device could not find requested video-format: " + std::to_string(format)));

    com_ptr<IDeckLinkDisplayMode> mode = wrap_raw<com_ptr>(m, true);

    BMDDisplayMode actualMode = bmdModeUnknown;
    BOOL           supported  = false;

    auto displayMode = mode->GetDisplayMode();
    if (FAILED(device->DoesSupportVideoMode(bmdVideoConnectionUnspecified,
                                            displayMode,
                                            pix_fmt,
                                            bmdNoVideoOutputConversion,
                                            flag,
                                            &actualMode,
                                            &supported)))
        CASPAR_THROW_EXCEPTION(caspar_exception()
                               << msg_info(L"Could not determine whether device supports requested video format: " +
                                           get_mode_name(mode)));
    else if (!supported)
        CASPAR_LOG(info) << L"Device may not support video-format: " << get_mode_name(mode);
    else if (actualMode != bmdModeUnknown && actualMode != displayMode)
        CASPAR_LOG(warning) << L"Device supports video-format with conversion: " << get_mode_name(mode);

    return mode;
}

void set_duplex(const com_iface_ptr<IDeckLinkAttributes_v10_11>&    attributes,
                const com_iface_ptr<IDeckLinkConfiguration_v10_11>& config,
                configuration::duplex_t                             duplex,
                const std::wstring&                                 print)
{
    BOOL supportsDuplexModeConfiguration;
    if (FAILED(
            attributes->GetFlag(static_cast<BMDDeckLinkAttributeID>(BMDDeckLinkSupportsDuplexModeConfiguration_v10_11),
                                &supportsDuplexModeConfiguration))) {
        CASPAR_LOG(error) << print
                          << L" Failed to set duplex mode, unable to check if card supports duplex mode setting.";
    }

    if (!supportsDuplexModeConfiguration) {
        CASPAR_LOG(warning) << print << L" This device does not support setting the duplex mode.";
        return;
    }

    std::map<configuration::duplex_t, BMDDuplexMode_v10_11> config_map{
        {configuration::duplex_t::full_duplex, bmdDuplexModeFull_v10_11},
        {configuration::duplex_t::half_duplex, bmdDuplexModeHalf_v10_11},
    };
    auto duplex_mode = config_map[duplex];

    if (FAILED(
            config->SetInt(static_cast<BMDDeckLinkConfigurationID>(bmdDeckLinkConfigDuplexMode_v10_11), duplex_mode))) {
        CASPAR_LOG(error) << print << L" Unable to set duplex mode.";
        return;
    }
    CASPAR_LOG(info) << print << L" Duplex mode set.";
}

void set_keyer(const com_iface_ptr<IDeckLinkProfileAttributes>& attributes,
               const com_iface_ptr<IDeckLinkOutput>&             output,
               const com_iface_ptr<IDeckLinkKeyer>&             decklink_keyer,
               configuration::keyer_t                           keyer,
               BMDDisplayMode                                   display_mode,
               BMDPixelFormat                                   pixel_format,
               const std::wstring&                              print)
{
    if (keyer == configuration::keyer_t::disabled_keyer) {
        CASPAR_LOG(info) << print << L" Keyer disabled.";
        return;
    }

    if (keyer == configuration::keyer_t::internal_keyer || keyer == configuration::keyer_t::external_keyer) {
        BMDDisplayMode actualMode = bmdModeUnknown;
        BOOL           supported  = FALSE;
        if (SUCCEEDED(output->DoesSupportVideoMode(bmdVideoConnectionUnspecified,
                                                   display_mode,
                                                   pixel_format,
                                                   bmdNoVideoOutputConversion,
                                                   bmdSupportedVideoModeKeying,
                                                   &actualMode,
                                                   &supported)) &&
            !supported) {
            CASPAR_LOG(warning) << print << L" Keying is not supported by this device for the current video mode. Disabling keyer.";
            decklink_keyer->Disable();
            return;
        }
    }

    if (keyer == configuration::keyer_t::internal_keyer) {
        BOOL value = true;
        if (SUCCEEDED(attributes->GetFlag(BMDDeckLinkSupportsInternalKeying, &value)) && !value)
            CASPAR_LOG(error) << print << L" Failed to enable internal keyer.";
        else if (FAILED(decklink_keyer->Enable(FALSE)))
            CASPAR_LOG(error) << print << L" Failed to enable internal keyer.";
        else if (FAILED(decklink_keyer->SetLevel(255)))
            CASPAR_LOG(error) << print << L" Failed to set key-level to max.";
        else
            CASPAR_LOG(info) << print << L" Enabled internal keyer.";
    } else if (keyer == configuration::keyer_t::external_keyer) {
        BOOL value = true;
        if (SUCCEEDED(attributes->GetFlag(BMDDeckLinkSupportsExternalKeying, &value)) && !value)
            CASPAR_LOG(error) << print << L" Failed to enable external keyer.";
        else if (FAILED(decklink_keyer->Enable(TRUE)))
            CASPAR_LOG(error) << print << L" Failed to enable external keyer.";
        else if (FAILED(decklink_keyer->SetLevel(255)))
            CASPAR_LOG(error) << print << L" Failed to set key-level to max.";
        else
            CASPAR_LOG(info) << print << L" Enabled external keyer.";
    }
}

core::video_format_desc get_decklink_format(const port_configuration&      config,
                                            const core::video_format_desc& fallback_format_desc)
{
    if (config.format.format != core::video_format::invalid && config.format.format != fallback_format_desc.format) {
        if (config.format.format != core::video_format::invalid && config.format.format != core::video_format::custom &&
            config.format.framerate * config.format.field_count ==
                fallback_format_desc.framerate * fallback_format_desc.field_count &&
            config.format.duration == fallback_format_desc.duration) {
            return config.format;
        }
    }

    if (fallback_format_desc.format == core::video_format::invalid ||
        fallback_format_desc.format == core::video_format::custom)
        CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"Decklink does not support the channel format"));

    return fallback_format_desc;
}

spl::shared_ptr<format_strategy> create_format_strategy(const configuration& config)
{
    if (config.hdr) {
        return create_hdr_v210_strategy(config.color_space);
    } else {
        return config.pixel_format == configuration::pixel_format_t::yuv ? create_sdr_v210_strategy(config.color_space)
                                                                         : create_sdr_bgra_strategy();
    }
}

enum EOTF
{
    SDR = 0,
    HDR = 1,
    PQ  = 2,
    HLG = 3
};

struct ChromaticityCoordinates
{
    double RedX;
    double RedY;
    double GreenX;
    double GreenY;
    double BlueX;
    double BlueY;
    double WhiteX;
    double WhiteY;
};

const auto REC_709  = ChromaticityCoordinates{0.640, 0.330, 0.300, 0.600, 0.150, 0.060, 0.3127, 0.3290};
const auto REC_2020 = ChromaticityCoordinates{0.708, 0.292, 0.170, 0.797, 0.131, 0.046, 0.3127, 0.3290};

class decklink_frame
    : public IDeckLinkVideoFrame
    , public IDeckLinkVideoFrameMetadataExtensions
{
    core::video_format_desc                      format_desc_;
    std::shared_ptr<void>                        data_;
    std::atomic<int>                             ref_count_{0};
    int                                          nb_samples_;
    const bool                                   hdr_;
    core::color_space                            color_space_;
    hdr_meta_configuration                       hdr_metadata_;
    BMDFrameFlags                                flags_;
    BMDPixelFormat                               pix_fmt_;
    int                                          row_bytes_;
    com_ptr<IDeckLinkVideoFrameAncillaryPackets> vanc_;

  public:
    decklink_frame(std::shared_ptr<void>         data,
                   core::video_format_desc       format_desc,
                   int                           nb_samples,
                   bool                          hdr,
                   BMDPixelFormat                pix_fmt,
                   int                           row_bytes,
                   bool                          vanc,
                   core::color_space             color_space,
                   const hdr_meta_configuration& hdr_metadata)
        : format_desc_(std::move(format_desc))
        , data_(std::move(data))
        , nb_samples_(nb_samples)
        , hdr_(hdr)
        , pix_fmt_(pix_fmt)
        , row_bytes_(row_bytes)
        , color_space_(color_space)
        , hdr_metadata_(hdr_metadata)
        , flags_(hdr ? bmdFrameFlagDefault | bmdFrameContainsHDRMetadata : bmdFrameFlagDefault)
        , vanc_(vanc ? create_ancillary_packets() : nullptr)
    {
    }

    // IUnknown

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID* ppv) override
    {
        if (ppv == nullptr)
            return E_INVALIDARG;

        REFIID iunknown = IID_IUnknown;

        if (std::memcmp(&iid, &iunknown, sizeof(REFIID)) == 0) {
            *ppv = this;
            AddRef();
        } else if (std::memcmp(&iid, &IID_IDeckLinkVideoFrame, sizeof(REFIID)) == 0) {
            *ppv = static_cast<IDeckLinkVideoFrame*>(this);
            AddRef();
        } else if (hdr_ && std::memcmp(&iid, &IID_IDeckLinkVideoFrameMetadataExtensions, sizeof(REFIID)) == 0) {
            *ppv = static_cast<IDeckLinkVideoFrameMetadataExtensions*>(this);
            AddRef();
        } else if (vanc_ && std::memcmp(&iid, &IID_IDeckLinkVideoFrameAncillaryPackets, sizeof(REFIID)) == 0) {
            auto raw = get_raw(vanc_);
            raw->AddRef();
            *ppv = raw;

        } else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }

        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_count_; }

    ULONG STDMETHODCALLTYPE Release() override
    {
        if (--ref_count_ == 0) {
            delete this;

            return 0;
        }

        return ref_count_;
    }

    // IDecklinkVideoFrame

    long STDMETHODCALLTYPE           GetWidth() override { return static_cast<long>(format_desc_.width); }
    long STDMETHODCALLTYPE           GetHeight() override { return static_cast<long>(format_desc_.height); }
    long STDMETHODCALLTYPE           GetRowBytes() override { return static_cast<long>(row_bytes_); }
    BMDPixelFormat STDMETHODCALLTYPE GetPixelFormat() override { return pix_fmt_; }
    BMDFrameFlags STDMETHODCALLTYPE  GetFlags() override { return flags_; }

    HRESULT STDMETHODCALLTYPE GetBytes(void** buffer) override
    {
        *buffer = data_.get();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetTimecode(BMDTimecodeFormat format, IDeckLinkTimecode** timecode) override
    {
        return S_FALSE;
    }

    HRESULT STDMETHODCALLTYPE GetAncillaryData(IDeckLinkVideoFrameAncillary** ancillary) override { return S_FALSE; }

    [[nodiscard]] int nb_samples() const { return nb_samples_; }

    // IDeckLinkVideoFrameMetadataExtensions
    HRESULT STDMETHODCALLTYPE GetInt(BMDDeckLinkFrameMetadataID metadataID, int64_t* value) override
    {
        HRESULT result = S_OK;

        switch (metadataID) {
            case bmdDeckLinkFrameMetadataHDRElectroOpticalTransferFunc:
                *value = EOTF::HLG;
                break;

            case bmdDeckLinkFrameMetadataColorspace:
                *value = (color_space_ == core::color_space::bt2020) ? bmdColorspaceRec2020 : bmdColorspaceRec709;
                break;

            default:
                value  = nullptr;
                result = E_INVALIDARG;
        }

        return result;
    }

    HRESULT STDMETHODCALLTYPE GetFloat(BMDDeckLinkFrameMetadataID metadataID, double* value) override
    {
        const auto color_space = (color_space_ == core::color_space::bt2020) ? &REC_2020 : &REC_709;
        HRESULT    result      = S_OK;

        switch (metadataID) {
            case bmdDeckLinkFrameMetadataHDRDisplayPrimariesRedX:
                *value = color_space->RedX;
                break;

            case bmdDeckLinkFrameMetadataHDRDisplayPrimariesRedY:
                *value = color_space->RedY;
                break;

            case bmdDeckLinkFrameMetadataHDRDisplayPrimariesGreenX:
                *value = color_space->GreenX;
                break;

            case bmdDeckLinkFrameMetadataHDRDisplayPrimariesGreenY:
                *value = color_space->GreenY;
                break;

            case bmdDeckLinkFrameMetadataHDRDisplayPrimariesBlueX:
                *value = color_space->BlueX;
                break;

            case bmdDeckLinkFrameMetadataHDRDisplayPrimariesBlueY:
                *value = color_space->BlueY;
                break;

            case bmdDeckLinkFrameMetadataHDRWhitePointX:
                *value = color_space->WhiteX;
                break;

            case bmdDeckLinkFrameMetadataHDRWhitePointY:
                *value = color_space->WhiteY;
                break;

            case bmdDeckLinkFrameMetadataHDRMaxDisplayMasteringLuminance:
                *value = hdr_metadata_.max_dml;
                break;

            case bmdDeckLinkFrameMetadataHDRMinDisplayMasteringLuminance:
                *value = hdr_metadata_.min_dml;
                break;

            case bmdDeckLinkFrameMetadataHDRMaximumContentLightLevel:
                *value = hdr_metadata_.max_cll;
                break;

            case bmdDeckLinkFrameMetadataHDRMaximumFrameAverageLightLevel:
                *value = hdr_metadata_.max_fall;
                break;

            default:
                value  = nullptr;
                result = E_INVALIDARG;
        }

        return result;
    }

    HRESULT STDMETHODCALLTYPE GetFlag(BMDDeckLinkFrameMetadataID, BOOL* value) override
    {
        // Not expecting GetFlag
        *value = false;
        return E_INVALIDARG;
    }

    HRESULT STDMETHODCALLTYPE GetString(BMDDeckLinkFrameMetadataID, String* value) override
    {
        // Not expecting GetString
        *value = nullptr;
        return E_INVALIDARG;
    }

    HRESULT STDMETHODCALLTYPE GetBytes(BMDDeckLinkFrameMetadataID metadataID,
                                       void*                      buffer,
                                       uint32_t*                  bufferSize) override
    {
        *bufferSize = 0;
        return E_INVALIDARG;
    }
};

struct decklink_secondary_port final : public IDeckLinkVideoOutputCallback
{
    const configuration                       config_;
    const port_configuration                  output_config_;
    spl::shared_ptr<format_strategy>          format_strategy_;
    com_ptr<IDeckLink>                        decklink_      = get_device(output_config_.device_index);
    com_iface_ptr<IDeckLinkOutput>            output_        = iface_cast<IDeckLinkOutput>(decklink_);
    com_iface_ptr<IDeckLinkKeyer>             keyer_         = iface_cast<IDeckLinkKeyer>(decklink_, true);
    com_iface_ptr<IDeckLinkProfileAttributes> attributes_    = iface_cast<IDeckLinkProfileAttributes>(decklink_);
    com_iface_ptr<IDeckLinkConfiguration>     configuration_ = iface_cast<IDeckLinkConfiguration>(decklink_);
    int                                       device_sync_group_;
    std::optional<core::const_frame>          first_field_;

    const std::wstring model_name_ = get_model_name(decklink_);

    // long long video_scheduled_ = 0;

    const core::video_format_desc channel_format_desc_;
    const core::video_format_desc decklink_format_desc_;
    com_ptr<IDeckLinkDisplayMode> mode_ = get_display_mode(output_,
                                                           decklink_format_desc_.format,
                                                           format_strategy_->get_pixel_format(),
                                                           bmdSupportedVideoModeDefault);

    decklink_secondary_port(const configuration&           config,
                            port_configuration             output_config,
                            core::video_format_desc        channel_format_desc,
                            const core::video_format_desc& main_decklink_format_desc,
                            const std::wstring&            print,
                            int                            device_sync_group)
        : config_(config)
        , output_config_(std::move(output_config))
        , format_strategy_(create_format_strategy(config))
        , device_sync_group_(device_sync_group)
        , channel_format_desc_(std::move(channel_format_desc))
        , decklink_format_desc_(get_decklink_format(output_config_, main_decklink_format_desc))
    {
        if (main_decklink_format_desc.format != decklink_format_desc_.format) {
            CASPAR_LOG(info) << print << L" Disabling sync group for output with different format.";
            device_sync_group_ = 0;
        }

        if (config.duplex != configuration::duplex_t::default_duplex) {
            set_duplex(iface_cast<IDeckLinkAttributes_v10_11>(decklink_),
                       iface_cast<IDeckLinkConfiguration_v10_11>(decklink_),
                       config.duplex,
                       print);
        }

        set_latency(configuration_, config.latency, print);
        set_keyer(attributes_, output_, keyer_, config.keyer, mode_->GetDisplayMode(), format_strategy_->get_pixel_format(), print);

        if (device_sync_group_ > 0 &&
            FAILED(configuration_->SetInt(bmdDeckLinkConfigPlaybackGroup, device_sync_group_))) {
            CASPAR_LOG(error) << print << L" Failed to enable sync group.";
            device_sync_group_ = 0;
        } else {
            CASPAR_LOG(trace) << print << L" Joined sync group " << device_sync_group;
        }

        if (FAILED(output_->SetScheduledFrameCompletionCallback(this)))
            CASPAR_THROW_EXCEPTION(caspar_exception()
                                   << msg_info(print + L" Failed to set key playback completion callback.")
                                   << boost::errinfo_api_function("SetScheduledFrameCompletionCallback"));
    }

    ~decklink_secondary_port()
    {
        if (output_) {
            if (device_sync_group_ == 0) {
                output_->StopScheduledPlayback(0, nullptr, 0);
            }

            output_->DisableVideoOutput();
        }
    }

    [[nodiscard]] std::wstring print() const
    {
        return model_name_ + L" [" + std::to_wstring(output_config_.device_index) + L"|" + decklink_format_desc_.name +
               L"]";
    }

    template <typename Print>
    void enable_video(const Print& print)
    {
        if (FAILED(output_->EnableVideoOutput(mode_->GetDisplayMode(),
                                              device_sync_group_ > 0 ? bmdVideoOutputSynchronizeToPlaybackGroup
                                                                     : bmdVideoOutputFlagDefault)))
            CASPAR_THROW_EXCEPTION(caspar_exception()
                                   << msg_info(print() + L" Could not enable secondary video output."));
    }

    template <typename Print>
    void start_playback(const Print& print)
    {
        if (device_sync_group_ == 0) {
            if (FAILED(output_->StartScheduledPlayback(0, decklink_format_desc_.time_scale, 1.0))) {
                CASPAR_THROW_EXCEPTION(caspar_exception()
                                       << msg_info(print() + L" Failed to schedule secondary playback."));
            }
        }
    }

    void schedule_frame(core::const_frame frame, BMDTimeValue display_time)
    {
        bool isInterlaced = decklink_format_desc_.field_count != 1;
        if (isInterlaced && !first_field_.has_value()) {
            // If this is interlaced it needs a pair of frames at a time
            first_field_ = frame;
            return;
        }

        // Figure out which frame is which
        core::const_frame frame1;
        core::const_frame frame2;
        if (isInterlaced) {
            frame1       = *first_field_;
            first_field_ = {};
            frame2       = frame;
        } else {
            frame1 = frame;
        }

        auto image_data = format_strategy_->convert_frame_for_port(
            channel_format_desc_, decklink_format_desc_, output_config_, frame1, frame2, mode_->GetFieldDominance());

        schedule_next_video(image_data, 0, display_time);
    }

    void schedule_next_video(std::shared_ptr<void> image_data, int nb_samples, BMDTimeValue display_time)
    {
        auto packed_frame = wrap_raw<com_ptr, IDeckLinkVideoFrame>(
            new decklink_frame(std::move(image_data),
                               decklink_format_desc_,
                               nb_samples,
                               config_.hdr,
                               format_strategy_->get_pixel_format(),
                               format_strategy_->get_row_bytes(decklink_format_desc_.width),
                               false,
                               config_.color_space,
                               config_.hdr_meta));
        if (FAILED(output_->ScheduleVideoFrame(get_raw(packed_frame),
                                               display_time,
                                               decklink_format_desc_.duration,
                                               decklink_format_desc_.time_scale))) {
            CASPAR_LOG(error) << print() << L" Failed to schedule primary video.";
        }

        // video_scheduled_ += decklink_format_desc_.duration;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, LPVOID*) override { return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE   AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE   Release() override { return 1; }

    HRESULT STDMETHODCALLTYPE ScheduledPlaybackHasStopped() override { return S_OK; }

    HRESULT STDMETHODCALLTYPE ScheduledFrameCompleted(IDeckLinkVideoFrame*           completed_frame,
                                                      BMDOutputFrameCompletionResult result) override
    {
        // Let the primary callback keep the pace, so no scheduling here.

        return S_OK;
    }
};

struct decklink_consumer final : public IDeckLinkVideoOutputCallback
{
    const int                        channel_index_;
    const configuration              config_;
    spl::shared_ptr<format_strategy> format_strategy_;

    com_ptr<IDeckLink>                        decklink_      = get_device(config_.primary.device_index);
    com_iface_ptr<IDeckLinkOutput>            output_        = iface_cast<IDeckLinkOutput>(decklink_);
    com_iface_ptr<IDeckLinkConfiguration>     configuration_ = iface_cast<IDeckLinkConfiguration>(decklink_);
    com_iface_ptr<IDeckLinkKeyer>             keyer_         = iface_cast<IDeckLinkKeyer>(decklink_, true);
    com_iface_ptr<IDeckLinkProfileAttributes> attributes_    = iface_cast<IDeckLinkProfileAttributes>(decklink_);

    std::mutex         exception_mutex_;
    std::exception_ptr exception_;

    const std::wstring            model_name_ = get_model_name(decklink_);
    const core::video_format_desc channel_format_desc_;
    const core::video_format_desc decklink_format_desc_;

    std::mutex                    buffer_mutex_;
    std::condition_variable       buffer_cond_;
    std::queue<core::const_frame> buffer_;
    int                           buffer_capacity_ = channel_format_desc_.field_count;

    const int buffer_size_ = config_.buffer_depth(); // Minimum buffer-size 3.

    long long video_scheduled_ = 0;
    long long audio_scheduled_ = 0;

    boost::circular_buffer<std::vector<int32_t>> audio_container_{static_cast<unsigned long>(buffer_size_ + 1)};

    spl::shared_ptr<diagnostics::graph> graph_;
    caspar::timer                       tick_timer_;
    reference_signal_detector           reference_signal_detector_{output_};
    // std::atomic<int64_t>                                  scheduled_frames_completed_{0};
    std::vector<std::unique_ptr<decklink_secondary_port>> secondary_port_contexts_;
    int                                                   device_sync_group_ = 0;

    com_ptr<IDeckLinkDisplayMode> mode_ = get_display_mode(output_,
                                                           decklink_format_desc_.format,
                                                           format_strategy_->get_pixel_format(),
                                                           bmdSupportedVideoModeDefault);

    std::atomic<bool>              abort_request_{false};
    std::shared_ptr<decklink_vanc> vanc_;

  public:
    decklink_consumer(const configuration& config, core::video_format_desc channel_format_desc, int channel_index)
        : channel_index_(channel_index)
        , config_(config)
        , format_strategy_(create_format_strategy(config))
        , channel_format_desc_(std::move(channel_format_desc))
        , decklink_format_desc_(get_decklink_format(config.primary, channel_format_desc_))
    {
        graph_->set_color("tick-time", diagnostics::color(0.0f, 0.6f, 0.9f));
        graph_->set_color("late-frame", diagnostics::color(0.6f, 0.3f, 0.3f));
        graph_->set_color("dropped-frame", diagnostics::color(0.3f, 0.6f, 0.3f));
        graph_->set_color("flushed-frame", diagnostics::color(0.4f, 0.3f, 0.8f));
        graph_->set_color("buffered-audio", diagnostics::color(0.9f, 0.9f, 0.5f));
        graph_->set_color("buffered-video", diagnostics::color(0.2f, 0.9f, 0.9f));

        if (config.duplex != configuration::duplex_t::default_duplex) {
            set_duplex(iface_cast<IDeckLinkAttributes_v10_11>(decklink_),
                       iface_cast<IDeckLinkConfiguration_v10_11>(decklink_),
                       config.duplex,
                       print());
        }

        /*
        if (key_context_) {
            graph_->set_color("key-offset", diagnostics::color(1.0f, 0.0f, 0.0f));
        }
        */

        graph_->set_text(print());
        diagnostics::register_graph(graph_);

        // If there are additional ports devices, then enable the sync group
        if (!config.secondaries.empty()) {
            // A unique id is needed for this group, this is simpler than a random number
            device_sync_group_ = static_cast<int>(config.primary.device_index);

            if (FAILED(configuration_->SetInt(bmdDeckLinkConfigPlaybackGroup, device_sync_group_))) {
                device_sync_group_ = 0;
                CASPAR_LOG(error) << print() << L" Failed to enable sync group.";
            } else {
                CASPAR_LOG(debug) << print() << L" Enabled sync group: " << device_sync_group_;
            }
        }

        // create the secondary ports
        for (auto& secondary_port_config : config_.secondaries) {
            secondary_port_contexts_.push_back(std::make_unique<decklink_secondary_port>(config,
                                                                                         secondary_port_config,
                                                                                         channel_format_desc_,
                                                                                         decklink_format_desc_,
                                                                                         print(),
                                                                                         device_sync_group_));
        }

        if (config.vanc.enable) {
            BOOL flag = TRUE;
            attributes_->GetFlag(BMDDeckLinkVANCRequires10BitYUVVideoFrames, &flag);
            if (flag) {
                CASPAR_LOG(warning) << print()
                                    << L" DeckLink hardware only supports VANC when the active picture and ancillary "
                                       L"data are both 10-bit YUV pixel format.";
            } else {
                CASPAR_LOG(info) << print() << L" DeckLink hardware supports VANC.";
                vanc_ = create_vanc(config.vanc);
            }
        }

        enable_video();

        if (config.embedded_audio) {
            enable_audio();
        }

        set_latency(configuration_, config.latency, print());
        set_keyer(attributes_, output_, keyer_, config.keyer, mode_->GetDisplayMode(), format_strategy_->get_pixel_format(), print());

        if (config.hdr) {
            BOOL flag = FALSE;
            if (SUCCEEDED(attributes_->GetFlag(BMDDeckLinkSupportsHDRMetadata, &flag)) && !flag)
                CASPAR_LOG(error) << print() << L" Device does not support HDR metadata.";
            if (SUCCEEDED(attributes_->GetFlag(BMDDeckLinkSupportsColorspaceMetadata, &flag)) && !flag)
                CASPAR_LOG(warning) << print() << L" Device does not support colorspace metadata.";
        }

        if (config.embedded_audio) {
            output_->BeginAudioPreroll();
        }

        for (int n = 0; n < buffer_size_; ++n) {
            auto nb_samples = decklink_format_desc_.audio_cadence[n % decklink_format_desc_.audio_cadence.size()] *
                              decklink_format_desc_.field_count;
            if (config.embedded_audio) {
                schedule_next_audio(std::vector<int32_t>(nb_samples * decklink_format_desc_.audio_channels),
                                    nb_samples);
            }

            std::shared_ptr<void> image_data = format_strategy_->allocate_frame_data(decklink_format_desc_);

            schedule_next_video(image_data, nb_samples, video_scheduled_);
            for (auto& context : secondary_port_contexts_) {
                context->schedule_next_video(image_data, 0, video_scheduled_);
            }

            video_scheduled_ += decklink_format_desc_.duration;
        }

        if (config.embedded_audio) {
            output_->EndAudioPreroll();
        }

        wait_for_reference_lock();

        start_playback();
    }

    ~decklink_consumer()
    {
        abort_request_ = true;
        buffer_cond_.notify_all();

        if (output_ != nullptr) {
            output_->StopScheduledPlayback(0, nullptr, 0);
            if (config_.embedded_audio) {
                output_->DisableAudioOutput();
            }
            output_->DisableVideoOutput();
        }

        secondary_port_contexts_.clear();
    }

    void wait_for_reference_lock()
    {
        if (config_.wait_for_reference_duration == 0 ||
            config_.wait_for_reference == configuration::wait_for_reference_t::disabled) {
            // Wait disabled
            return;
        }
        if (config_.wait_for_reference == configuration::wait_for_reference_t::automatic && device_sync_group_ == 0) {
            // Wait is not necessary
            return;
        }

        CASPAR_LOG(info) << print() << L" Reference signal: waiting for lock";

        // When using the sync group we need a reference lock before starting playback, otherwise the outputs will not
        // be locked correctly and will be out of sync
        auto wait_end = std::chrono::system_clock::now() + std::chrono::seconds(config_.wait_for_reference_duration);
        while (std::chrono::system_clock::now() < wait_end) {
            BMDReferenceStatus reference_status;
            if (output_->GetReferenceStatus(&reference_status) != S_OK) {
                CASPAR_LOG(error) << print() << L" Reference signal: failed while querying status";
                break;
            }

            if (reference_status & bmdReferenceNotSupportedByHardware) {
                CASPAR_LOG(info) << print() << L" Reference signal: not supported by hardware.";
                break;
            } else if (reference_status & bmdReferenceLocked) {
                CASPAR_LOG(info) << print() << L" Reference signal: locked";

                // TODO - is this necessary? This is to give it a chance to stabilise before continuing
                std::this_thread::sleep_for(std::chrono::milliseconds(100));

                return;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        CASPAR_LOG(warning) << print() << L" Reference signal: unable to acquire lock";
    }

    void enable_audio()
    {
        if (FAILED(output_->EnableAudioOutput(bmdAudioSampleRate48kHz,
                                              bmdAudioSampleType32bitInteger,
                                              decklink_format_desc_.audio_channels,
                                              bmdAudioOutputStreamTimestamped))) {
            CASPAR_THROW_EXCEPTION(caspar_exception() << msg_info(print() + L" Could not enable audio output."));
        }

        CASPAR_LOG(info) << print() << L" Enabled embedded-audio.";
    }

    void enable_video()
    {
        BMDVideoOutputFlags output_flags = bmdVideoOutputFlagDefault;
        if (device_sync_group_ > 0) {
            output_flags = static_cast<BMDVideoOutputFlags>(output_flags | bmdVideoOutputSynchronizeToPlaybackGroup);
        }
        if (vanc_) {
            output_flags = static_cast<BMDVideoOutputFlags>(output_flags | bmdVideoOutputVANC);
        }

        if (FAILED(output_->EnableVideoOutput(mode_->GetDisplayMode(), output_flags))) {
            CASPAR_THROW_EXCEPTION(caspar_exception()
                                   << msg_info(print() + L" Could not enable primary video output."));
        }

        if (FAILED(output_->SetScheduledFrameCompletionCallback(this))) {
            CASPAR_THROW_EXCEPTION(caspar_exception()
                                   << msg_info(print() + L" Failed to set primary playback completion callback.")
                                   << boost::errinfo_api_function("SetScheduledFrameCompletionCallback"));
        }

        for (auto& context : secondary_port_contexts_) {
            context->enable_video([this]() { return print(); });
        }
    }

    void start_playback()
    {
        if (FAILED(output_->StartScheduledPlayback(0, decklink_format_desc_.time_scale, 1.0))) {
            CASPAR_THROW_EXCEPTION(caspar_exception() << msg_info(print() + L" Failed to schedule primary playback."));
        }

        for (auto& context : secondary_port_contexts_) {
            context->start_playback([this]() { return print(); });
        }
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, LPVOID*) override { return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE   AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE   Release() override { return 1; }

    HRESULT STDMETHODCALLTYPE ScheduledPlaybackHasStopped() override
    {
        CASPAR_LOG(info) << print() << L" Scheduled playback has stopped.";
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE ScheduledFrameCompleted(IDeckLinkVideoFrame*           completed_frame,
                                                      BMDOutputFrameCompletionResult result) override
    {
        thread_local auto priority_set = false;
        if (!priority_set) {
            priority_set = true;
            // set_thread_realtime_priority();
            set_thread_name(L"decklink_consumer[" + std::to_wstring(config_.primary.device_index) +
                            L"]-ScheduledFrameCompleted");
        }
        try {
            auto tick_time = tick_timer_.elapsed() * decklink_format_desc_.hz * 0.5;
            graph_->set_value("tick-time", tick_time);
            tick_timer_.restart();

            reference_signal_detector_.detect_change([this]() { return print(); });

            auto dframe = reinterpret_cast<decklink_frame*>(completed_frame);
            // ++scheduled_frames_completed_;

            /*
            if (key_context_) {
                graph_->set_value(
                    "key-offset",
                    static_cast<double>(scheduled_frames_completed_ - key_context_->scheduled_frames_completed_) * 0.1 +
                        0.5);
            }
            */

            /**
             * TODO - track how the secondaries are doing by comparing IDeckLinkOutput::GetScheduledStreamTime
             */

            if (result == bmdOutputFrameDisplayedLate) {
                graph_->set_tag(diagnostics::tag_severity::WARNING, "late-frame");
                video_scheduled_ += decklink_format_desc_.duration;
                audio_scheduled_ += dframe->nb_samples();
            } else if (result == bmdOutputFrameDropped) {
                graph_->set_tag(diagnostics::tag_severity::WARNING, "dropped-frame");
            } else if (result == bmdOutputFrameFlushed) {
                graph_->set_tag(diagnostics::tag_severity::WARNING, "flushed-frame");
            }

            {
                UINT32 buffered;
                output_->GetBufferedVideoFrameCount(&buffered);
                graph_->set_value("buffered-video", static_cast<double>(buffered) / config_.buffer_depth());

                if (config_.embedded_audio) {
                    output_->GetBufferedAudioSampleFrameCount(&buffered);
                    graph_->set_value("buffered-audio",
                                      static_cast<double>(buffered) /
                                          (decklink_format_desc_.audio_cadence[0] * decklink_format_desc_.field_count *
                                           config_.buffer_depth()));
                }
            }

            core::const_frame frame1 = pop();
            core::const_frame frame2;

            bool isInterlaced = mode_->GetFieldDominance() != bmdProgressiveFrame;
            if (mode_->GetFieldDominance() != bmdProgressiveFrame) {
                // If the main is not progressive, then pop the second frame
                frame2 = pop();
            }

            if (abort_request_)
                return E_FAIL;

            BMDTimeValue video_display_time = video_scheduled_;
            video_scheduled_ += decklink_format_desc_.duration;

            std::vector<std::int32_t> audio_data;
            if (config_.embedded_audio) {
                audio_data.insert(audio_data.end(), frame1.audio_data().begin(), frame1.audio_data().end());
                if (isInterlaced) {
                    audio_data.insert(audio_data.end(), frame2.audio_data().begin(), frame2.audio_data().end());
                }
            }
            // TODO: is this reliable?
            const int nb_samples = static_cast<int>(audio_data.size()) / decklink_format_desc_.audio_channels;

            // Schedule video
            tbb::parallel_for(-1, static_cast<int>(secondary_port_contexts_.size()), [&](int i) {
                if (i == -1) {
                    // Primary port
                    std::shared_ptr<void> image_data =
                        format_strategy_->convert_frame_for_port(channel_format_desc_,
                                                                 decklink_format_desc_,
                                                                 config_.primary,
                                                                 frame1,
                                                                 frame2,
                                                                 mode_->GetFieldDominance());

                    schedule_next_video(image_data, nb_samples, video_display_time);

                    if (config_.embedded_audio) {
                        schedule_next_audio(std::move(audio_data), nb_samples);
                    }
                } else {
                    // Send frame to secondary ports
                    auto& context = secondary_port_contexts_[i];
                    context->schedule_frame(frame1, video_display_time);
                    if (isInterlaced) {
                        context->schedule_frame(frame2, video_display_time);
                    }

                    if (config_.embedded_audio) {
                        // TODO - audio for secondaries?
                    }
                }
            });

        } catch (...) {
            std::lock_guard<std::mutex> lock(exception_mutex_);
            exception_ = std::current_exception();
            return E_FAIL;
        }

        return S_OK;
    }

    core::const_frame pop()
    {
        core::const_frame frame;
        {
            std::unique_lock<std::mutex> lock(buffer_mutex_);
            buffer_cond_.wait(lock, [&] { return !buffer_.empty() || abort_request_; });
            if (!abort_request_) {
                frame = buffer_.front();
                buffer_.pop();
            }
        }
        buffer_cond_.notify_all();
        return frame;
    }

    void schedule_next_audio(std::vector<std::int32_t> audio, int nb_samples)
    {
        // TODO (refactor) does ScheduleAudioSamples copy data?

        audio_container_.push_back(std::move(audio));

        if (FAILED(output_->ScheduleAudioSamples(audio_container_.back().data(),
                                                 nb_samples,
                                                 audio_scheduled_,
                                                 decklink_format_desc_.audio_sample_rate,
                                                 nullptr))) {
            CASPAR_LOG(error) << print() << L" Failed to schedule audio.";
        }

        audio_scheduled_ += nb_samples; // TODO - what if there are too many/few samples in this frame?
    }

    void schedule_next_video(std::shared_ptr<void> image_data, int nb_samples, BMDTimeValue display_time)
    {
        auto fmt        = format_strategy_->get_pixel_format();
        auto row_bytes  = format_strategy_->get_row_bytes(decklink_format_desc_.width);
        auto fill_frame = wrap_raw<com_ptr, IDeckLinkVideoFrame>(new decklink_frame(std::move(image_data),
                                                                                    decklink_format_desc_,
                                                                                    nb_samples,
                                                                                    config_.hdr,
                                                                                    fmt,
                                                                                    row_bytes,
                                                                                    config_.vanc.enable,
                                                                                    config_.color_space,
                                                                                    config_.hdr_meta));

        if (vanc_ && vanc_->has_data()) {
            auto ancillary_packets = iface_cast<IDeckLinkVideoFrameAncillaryPackets>(fill_frame);
            auto packets           = vanc_->pop_packets();
            for (auto& packet : packets) {
                if (FAILED(ancillary_packets->AttachPacket(get_raw(packet)))) {
                    CASPAR_LOG(error) << print() << L" Failed to add ancillary packet.";
                }
            }

            bool isInterlaced = mode_->GetFieldDominance() != bmdProgressiveFrame;
            if (isInterlaced) {
                auto field2_packets = vanc_->pop_packets(true);
                for (auto& packet : field2_packets) {
                    if (FAILED(ancillary_packets->AttachPacket(get_raw(packet)))) {
                        CASPAR_LOG(error) << print() << L" Failed to add ancillary packet.";
                    }
                }
            }
        }
        if (FAILED(output_->ScheduleVideoFrame(
                get_raw(fill_frame), display_time, decklink_format_desc_.duration, decklink_format_desc_.time_scale))) {
            CASPAR_LOG(error) << print() << L" Failed to schedule primary video.";
        }
    }

    bool send(core::video_field field, core::const_frame frame)
    {
        {
            std::lock_guard<std::mutex> lock(exception_mutex_);
            if (exception_ != nullptr) {
                std::rethrow_exception(exception_);
            }
        }

        if (frame) {
            std::unique_lock<std::mutex> lock(buffer_mutex_);
            if (field != core::video_field::b) {
                // Always push a field2, as we have supplied field1
                buffer_cond_.wait(lock, [&] { return buffer_.size() < buffer_capacity_ || abort_request_; });
            }
            buffer_.push(std::move(frame));
        }
        buffer_cond_.notify_all();

        return !abort_request_;
    }

    bool call(const std::vector<std::wstring>& params)
    {
        if (!vanc_) {
            CASPAR_LOG(warning) << print() << L" VANC is not enabled, ignoring: "
                                << (params.empty() ? L"N/A" : params[0]);
            return false;
        }

        try {
            bool result = vanc_->try_push_data(params);
            if (!result) {
                CASPAR_LOG(warning) << print() << L" Unknown command: " << (params.empty() ? L"N/A" : params[0]);
            }

            return result;
        } catch (...) {
            CASPAR_LOG(warning) << print() << L" Failed to apply: " << (params.empty() ? L"N/A" : params[0]);
        }
        return false;
    }

    [[nodiscard]] std::wstring print() const
    {
        std::wstringstream buffer;

        buffer << model_name_ << L" [" + std::to_wstring(channel_index_) << L"-"
               << std::to_wstring(config_.primary.device_index) << L"|" << decklink_format_desc_.name << L"]";

        for (auto& context : secondary_port_contexts_) {
            buffer << L" && " + context->print();
        }

        return buffer.str();
    }
};

struct decklink_consumer_proxy : public core::frame_consumer
{
    const configuration                config_;
    std::unique_ptr<decklink_consumer> consumer_;
    core::video_format_desc            format_desc_;
    executor                           executor_;

  public:
    explicit decklink_consumer_proxy(const configuration& config)
        : config_(config)
        , executor_(L"decklink_consumer[" + std::to_wstring(config.primary.device_index) + L"]")
    {
        executor_.begin_invoke([=] { com_initialize(); });
    }

    ~decklink_consumer_proxy() override
    {
        executor_.invoke([=, this] {
            set_thread_realtime_priority();
            consumer_.reset();
            com_uninitialize();
        });
    }

    void initialize(const core::video_format_desc& format_desc,
                    const core::channel_info&      channel_info,
                    int                            port_index) override
    {
        format_desc_ = format_desc;
        executor_.invoke([=, this] {
            consumer_.reset();
            consumer_ = std::make_unique<decklink_consumer>(config_, format_desc, channel_info.index);
        });
    }

    std::future<bool> send(core::video_field field, core::const_frame frame) override
    {
        return executor_.begin_invoke([=, this] { return consumer_->send(field, frame); });
    }

    std::future<bool> call(const std::vector<std::wstring>& params) override
    {
        return executor_.begin_invoke([=, this] { return consumer_->call(params); });
    }

    [[nodiscard]] std::wstring print() const override
    {
        auto id_str = consumer_id().empty() ? L"" : L"|id=" + consumer_id();
        return consumer_ ? consumer_->print() + id_str : L"[decklink_consumer" + id_str + L"]";
    }

    [[nodiscard]] std::wstring name() const override { return L"decklink"; }

    [[nodiscard]] int index() const override { return 300 + static_cast<int>(config_.primary.device_index); }

    [[nodiscard]] bool has_synchronization_clock() const override { return true; }

    [[nodiscard]] core::monitor::state state() const override
    {
        auto state = get_state_for_config(config_, format_desc_);
        if (!consumer_id().empty())
            state["decklink/consumer_id"] = consumer_id();
        return state;
    }
};

spl::shared_ptr<core::frame_consumer> create_consumer(const std::vector<std::wstring>&     params,
                                                      const core::video_format_repository& format_repository,
                                                      const std::vector<spl::shared_ptr<core::video_channel>>& channels,
                                                      const core::channel_info& channel_info)
{
    if (params.empty() || !boost::iequals(params.at(0), L"DECKLINK")) {
        return core::frame_consumer::empty();
    }

    configuration config = parse_amcp_config(params, format_repository, channel_info);

    config.hdr = (channel_info.depth != common::bit_depth::bit8);

    if (config.hdr && config.primary.key_only) {
        CASPAR_THROW_EXCEPTION(caspar_exception()
                               << msg_info("Decklink consumer does not support hdr in combination with key only"));
    }

    return spl::make_shared<decklink_consumer_proxy>(config);
}

spl::shared_ptr<core::frame_consumer>
create_preconfigured_consumer(const boost::property_tree::wptree&                      ptree,
                              const core::video_format_repository&                     format_repository,
                              const std::vector<spl::shared_ptr<core::video_channel>>& channels,
                              const core::channel_info&                                channel_info)
{
    configuration config = parse_xml_config(ptree, format_repository, channel_info);

    config.hdr = (channel_info.depth != common::bit_depth::bit8);

    if (config.hdr && config.primary.key_only) {
        CASPAR_THROW_EXCEPTION(caspar_exception()
                               << msg_info("Decklink consumer does not support hdr in combination with key only"));
    }

    return spl::make_shared<decklink_consumer_proxy>(config);
}

}} // namespace caspar::decklink

/*
##############################################################################
Pre-rolling

Mail: 2011-05-09

Yoshan
BMD Developer Support
developer@blackmagic-design.com

-----------------------------------------------------------------------------

Thanks for your inquiry. The minimum number of frames that you can preroll
for scheduled playback is three frames for video and four frames for audio.
As you mentioned if you preroll less frames then playback will not start or
playback will be very sporadic. From our experience with Media Express, we
recommended that at least seven frames are prerolled for smooth playback.

Regarding the bmdDeckLinkConfigLowLatencyVideoOutput flag:
There can be around 3 frames worth of latency on scheduled output.
When the bmdDeckLinkConfigLowLatencyVideoOutput flag is used this latency is
reduced  or removed for scheduled playback. If the DisplayVideoFrameSync()
method is used, the bmdDeckLinkConfigLowLatencyVideoOutput setting will
guarantee that the provided frame will be output as soon the previous
frame output has been completed.
################################################################################
*/

/*
##############################################################################
Async DMA Transfer without redundant copying

Mail: 2011-05-10

Yoshan
BMD Developer Support
developer@blackmagic-design.com

-----------------------------------------------------------------------------

Thanks for your inquiry. You could try subclassing IDeckLinkMutableVideoFrame
and providing a pointer to your video buffer when GetBytes() is called.
This may help to keep copying to a minimum. Please ensure that the pixel
format is in bmdFormat10BitYUV, otherwise the DeckLink API / driver will
have to colourspace convert which may result in additional copying.
################################################################################
*/
