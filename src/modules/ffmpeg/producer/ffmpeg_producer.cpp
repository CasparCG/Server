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

#include "ffmpeg_producer.h"

#include "av_producer.h"

#include <common/env.h>
#include <common/os/filesystem.h>
#include <common/param.h>

#include <core/frame/draw_frame.h>
#include <core/frame/frame_factory.h>
#include <core/producer/frame_producer.h>
#include <core/video_format.h>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <boost/logic/tribool.hpp>

#include <cstdint>

#pragma warning(push, 1)

extern "C" {
#define __STDC_CONSTANT_MACROS
#define __STDC_LIMIT_MACROS
#include <libavformat/avformat.h>
}

#pragma warning(pop)

namespace caspar { namespace ffmpeg {

using namespace std::chrono_literals;

struct ffmpeg_producer : public core::frame_producer
{
    const std::wstring                   filename_;
    spl::shared_ptr<core::frame_factory> frame_factory_;
    core::video_format_desc              format_desc_;

    std::shared_ptr<AVProducer> producer_;

  public:
    explicit ffmpeg_producer(spl::shared_ptr<core::frame_factory> frame_factory,
                             core::video_format_desc              format_desc,
                             std::wstring                         path,
                             std::wstring                         filename,
                             std::wstring                         vfilter,
                             std::wstring                         afilter,
                             boost::optional<int64_t>             start,
                             boost::optional<int64_t>             duration,
                             boost::optional<bool>                loop,
                             int                                  seekable)
        : filename_(filename)
        , frame_factory_(frame_factory)
        , format_desc_(format_desc)
        , producer_(new AVProducer(frame_factory_,
                                   format_desc_,
                                   u8(path),
                                   u8(filename),
                                   u8(vfilter),
                                   u8(afilter),
                                   start,
                                   duration,
                                   loop,
                                   seekable))
    {
    }

    ~ffmpeg_producer()
    {
        std::thread([producer = std::move(producer_)]() mutable {
            try {
                producer.reset();
            } catch (...) {
                CASPAR_LOG_CURRENT_EXCEPTION();
            }
        })
            .detach();
    }

    // frame_producer

    core::draw_frame last_frame() override { return producer_->prev_frame(); }

    core::draw_frame receive_impl(int nb_samples) override { return producer_->next_frame(); }

    std::uint32_t frame_number() const override
    {
        return static_cast<std::uint32_t>(producer_->time() - producer_->start());
    }

    std::uint32_t nb_frames() const override
    {
        return producer_->loop() ? std::numeric_limits<std::uint32_t>::max()
                                 : static_cast<std::uint32_t>(producer_->duration());
    }

    std::future<std::wstring> call(const std::vector<std::wstring>& params) override
    {
        std::wstring result;

        std::wstring cmd = params.at(0);
        std::wstring value;
        if (params.size() > 1) {
            value = params.at(1);
        }

        if (boost::iequals(cmd, L"loop")) {
            if (!value.empty()) {
                producer_->loop(boost::lexical_cast<bool>(value));
            }

            result = std::to_wstring(producer_->loop());
        } else if (boost::iequals(cmd, L"in") || boost::iequals(cmd, L"start")) {
            if (!value.empty()) {
                producer_->start(boost::lexical_cast<int64_t>(value));
            }

            result = std::to_wstring(producer_->start());
        } else if (boost::iequals(cmd, L"out")) {
            if (!value.empty()) {
                producer_->duration(boost::lexical_cast<int64_t>(value) - producer_->start());
            }

            result = std::to_wstring(producer_->start() + producer_->duration());
        } else if (boost::iequals(cmd, L"length")) {
            if (!value.empty()) {
                producer_->duration(boost::lexical_cast<std::int64_t>(value));
            }

            result = std::to_wstring(producer_->duration());
        } else if (boost::iequals(cmd, L"seek") && !value.empty()) {
            int64_t seek;
            if (boost::iequals(value, L"rel")) {
                seek = producer_->time();
            } else if (boost::iequals(value, L"in")) {
                seek = producer_->start();
            } else if (boost::iequals(value, L"out")) {
                seek = producer_->start() + producer_->duration();
            } else if (boost::iequals(value, L"end")) {
                seek = producer_->duration();
            } else {
                seek = boost::lexical_cast<int64_t>(value);
            }

            if (params.size() > 2) {
                seek += boost::lexical_cast<int64_t>(params.at(2));
            }

            producer_->seek(seek);

            result = std::to_wstring(seek);
        } else {
            CASPAR_THROW_EXCEPTION(invalid_argument());
        }

        std::promise<std::wstring> promise;
        promise.set_value(result);
        return promise.get_future();
    }

    std::wstring print() const override
    {
        const int64_t position = std::max(producer_->time() - producer_->start(), INT64_C(0));
        return L"ffmpeg[" + filename_ + L"|" + std::to_wstring(position) + L"/" +
               std::to_wstring(producer_->duration()) + L"]";
    }

    std::wstring name() const override { return L"ffmpeg"; }

    core::monitor::state state() const override { return producer_->state(); }
};

boost::tribool has_valid_extension(const std::wstring& filename)
{
    static const auto invalid_exts = {L".tga",
                                      L".tiff",
                                      L".tif",
                                      L".jp2",
                                      L".jpx",
                                      L".j2k",
                                      L".j2c",
                                      L".swf",
                                      L".ct",
                                      L".html",
                                      L".htm"};
    static const auto valid_exts   = {L".m2t",  L".m2ts",   L".mov",  L".mp4", L".dv",  L".flv", L".mpg",  L".dnxhd",
                                    L".h264", L".prores", L".mkv",  L".mxf", L".ts",  L".mp3", L".wav",  L".wma",
                                    L".nut",  L".flac",   L".opus", L".ogg", L".ogv", L".oga", L".webm", L".webp"};

    auto ext = boost::to_lower_copy(boost::filesystem::path(filename).extension().wstring());

    if (std::find(valid_exts.begin(), valid_exts.end(), ext) != valid_exts.end()) {
        return boost::tribool(true);
    }

    if (std::find(invalid_exts.begin(), invalid_exts.end(), ext) != invalid_exts.end()) {
        return boost::tribool(false);
    }
    return boost::tribool(boost::indeterminate);
}

bool has_invalid_protocol(const std::wstring& filename)
{
    static const auto invalid_protocols = {L"ndi:"};

    auto protocol = boost::to_lower_copy(boost::filesystem::path(filename).root_name().wstring());

    if (std::find(invalid_protocols.begin(), invalid_protocols.end(), protocol) != invalid_protocols.end()) {
        return true;
    }
    return false;
}

bool is_readable(const std::wstring& filename)
{
    auto u8filename = u8(filename);

    std::ifstream file(u8filename);
    if (file) {
        return true;
    }
    return false;
}

bool is_valid_file(const std::wstring& filename)
{
    if (!is_readable(filename)) {
        return false;
    }

    const auto valid_ext = has_valid_extension(filename);
    if (valid_ext) {
        return true;
    }
    if (!valid_ext) {
        return false;
    }

    auto u8filename = u8(filename);

    int         score = 0;
    AVProbeData pb    = {};
    pb.filename       = u8filename.c_str();

    if (av_probe_input_format2(&pb, false, &score) != nullptr) {
        return true;
    }

    std::ifstream file(u8filename);

    std::vector<unsigned char> buf;
    for (auto file_it = std::istreambuf_iterator<char>(file);
         file_it != std::istreambuf_iterator<char>() && buf.size() < 1024;
         ++file_it) {
        buf.push_back(*file_it);
    }

    if (buf.empty()) {
        return false;
    }

    pb.buf      = buf.data();
    pb.buf_size = static_cast<int>(buf.size());

    return av_probe_input_format2(&pb, true, &score) != nullptr;
}

std::wstring probe_stem(const std::wstring& stem)
{
    auto stem2  = boost::filesystem::path(stem);
    auto parent = find_case_insensitive(stem2.parent_path().wstring());

    if (!parent)
        return L"";

    auto dir = boost::filesystem::path(*parent);

    for (auto it = boost::filesystem::directory_iterator(dir); it != boost::filesystem::directory_iterator(); ++it) {
        if (boost::iequals(it->path().stem().wstring(), stem2.filename().wstring()) &&
            is_valid_file(it->path().wstring()))
            return it->path().wstring();
    }
    return L"";
}

spl::shared_ptr<core::frame_producer> create_producer(const core::frame_producer_dependencies& dependencies,
                                                      const std::vector<std::wstring>&         params)
{
    auto name = params.at(0);
    auto path = name;

    if (!boost::contains(path, L"://")) {
        auto mediaPath     = env::media_folder() + L"/" + path;
        auto fullMediaPath = find_case_insensitive(mediaPath);
        if (fullMediaPath && is_valid_file(*fullMediaPath)) {
            path = *fullMediaPath;
        } else {
            path = boost::filesystem::path(probe_stem(mediaPath)).generic_wstring();
            name += boost::filesystem::path(path).extension().wstring();
        }
    } else if (!has_valid_extension(path) || has_invalid_protocol(path)) {
        return core::frame_producer::empty();
    }

    if (path.empty()) {
        return core::frame_producer::empty();
    }

    auto seekable = get_param(L"SEEKABLE", params, static_cast<int>(2));

    auto loop = contains_param(L"LOOP", params);

    auto in = get_param(L"SEEK", params, static_cast<uint32_t>(0)); // compatibility
    in      = get_param(L"IN", params, in);

    auto out = get_param(L"LENGTH", params, std::numeric_limits<uint32_t>::max());
    if (out < std::numeric_limits<uint32_t>::max() - in)
        out += in;
    else
        out = std::numeric_limits<uint32_t>::max();
    out = get_param(L"OUT", params, out);

    auto filter_str = get_param(L"FILTER", params, L"");

    boost::ireplace_all(filter_str, L"DEINTERLACE_BOB", L"YADIF=1:-1");
    boost::ireplace_all(filter_str, L"DEINTERLACE_LQ", L"SEPARATEFIELDS");
    boost::ireplace_all(filter_str, L"DEINTERLACE", L"YADIF=0:-1");

    boost::optional<std::int64_t> start;
    boost::optional<std::int64_t> duration;

    if (in != 0) {
        start = in;
    }

    if (out != std::numeric_limits<uint32_t>::max()) {
        duration = out - in;
    }

    // TODO (fix) use raw input?
    auto vfilter = boost::to_lower_copy(get_param(L"VF", params, filter_str));
    auto afilter = boost::to_lower_copy(get_param(L"AF", params, get_param(L"FILTER", params, L"")));

    try {
        auto producer = spl::make_shared<ffmpeg_producer>(
            dependencies.frame_factory, dependencies.format_desc, name, path, vfilter, afilter, start, duration, loop, seekable);
        return core::create_destroy_proxy(std::move(producer));
    } catch (...) {
        CASPAR_LOG_CURRENT_EXCEPTION();
    }
    return core::frame_producer::empty();
}

}} // namespace caspar::ffmpeg
