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

#include "../ffmpeg.h"
#include "../ffmpeg_error.h"
#include "util/util.h"
#include "input/input.h"
#include "audio/audio_decoder.h"
#include "video/video_decoder.h"
#include "muxer/frame_muxer.h"
#include "filter/audio_filter.h"

#include <common/param.h>
#include <common/diagnostics/graph.h>
#include <common/future.h>

#include <core/frame/draw_frame.h>
#include <core/producer/framerate/framerate_producer.h>
#include <core/frame/frame_factory.h>

#include <future>
#include <queue>

namespace caspar { namespace ffmpeg {
struct seek_out_of_range : virtual user_error {};

std::wstring get_relative_or_original(
		const std::wstring& filename,
		const boost::filesystem::path& relative_to)
{
	boost::filesystem::path file(filename);
	auto result = file.filename().wstring();

	boost::filesystem::path current_path = file;

	while (true)
	{
		current_path = current_path.parent_path();

		if (boost::filesystem::equivalent(current_path, relative_to))
			break;

		if (current_path.empty())
			return filename;

		result = current_path.filename().wstring() + L"/" + result;
	}

	return result;
}

struct ffmpeg_producer : public core::frame_producer_base
{
	spl::shared_ptr<core::monitor::subject>				monitor_subject_;
	const std::wstring									filename_;
	const std::wstring									path_relative_to_media_		= get_relative_or_original(filename_, env::media_folder());

	const spl::shared_ptr<diagnostics::graph>			graph_;
	timer												frame_timer_;

	const spl::shared_ptr<core::frame_factory>			frame_factory_;

	core::constraints									constraints_;

	input												input_;
	std::unique_ptr<video_decoder>						video_decoder_;
	std::vector<std::unique_ptr<audio_decoder>>			audio_decoders_;
	std::unique_ptr<frame_muxer>						muxer_;

	const boost::rational<int>							framerate_;

	core::draw_frame									last_frame_;

	std::queue<std::pair<core::draw_frame, uint32_t>>	frame_buffer_;

	int64_t												frame_number_				= 0;
	uint32_t											file_frame_number_			= 0;
public:
	explicit ffmpeg_producer(
			const spl::shared_ptr<core::frame_factory>& frame_factory,
			const core::video_format_desc& format_desc,
			const std::wstring& url_or_file,
			const std::wstring& filter,
			bool loop,
			uint32_t in,
			uint32_t out,
			const std::wstring& custom_channel_order,
			const ffmpeg_options& vid_params)
		: filename_(url_or_file)
		, frame_factory_(frame_factory)
		, input_(graph_, url_or_file, loop, in, out, vid_params)
		, framerate_(read_framerate(*input_.context(), format_desc.framerate))
		, last_frame_(core::draw_frame::empty())
	{
		graph_->set_color("frame-time", diagnostics::color(0.1f, 1.0f, 0.1f));
		graph_->set_color("underflow", diagnostics::color(0.6f, 0.3f, 0.9f));
		diagnostics::register_graph(graph_);

		try
		{
			video_decoder_.reset(new video_decoder(input_.context()));
			CASPAR_LOG(info) << print() << L" " << video_decoder_->print();

			constraints_.width.set(video_decoder_->width());
			constraints_.height.set(video_decoder_->height());
		}
		catch (averror_stream_not_found&)
		{
			//CASPAR_LOG(warning) << print() << " No video-stream found. Running without video.";
		}
		catch (...)
		{
			CASPAR_LOG_CURRENT_EXCEPTION();
			CASPAR_LOG(warning) << print() << "Failed to open video-stream. Running without video.";
		}

		auto channel_layout = core::audio_channel_layout::invalid();
		std::vector<audio_input_pad> audio_input_pads;


		for (unsigned stream_index = 0; stream_index < input_.context()->nb_streams; ++stream_index)
		{
			auto stream = input_.context()->streams[stream_index];

			if (stream->codec->codec_type != AVMediaType::AVMEDIA_TYPE_AUDIO)
				continue;

			try
			{
				audio_decoders_.push_back(std::unique_ptr<audio_decoder>(new audio_decoder(stream_index, input_.context(), format_desc.audio_sample_rate)));
				audio_input_pads.emplace_back(
						boost::rational<int>(1, format_desc.audio_sample_rate),
						format_desc.audio_sample_rate,
						AVSampleFormat::AV_SAMPLE_FMT_S32,
						audio_decoders_.back()->ffmpeg_channel_layout());
				CASPAR_LOG(info) << print() << L" " << audio_decoders_.back()->print();
			}
			catch (averror_stream_not_found&)
			{
				//CASPAR_LOG(warning) << print() << " No audio-stream found. Running without audio.";
			}
			catch (...)
			{
				CASPAR_LOG_CURRENT_EXCEPTION();
				CASPAR_LOG(warning) << print() << " Failed to open audio-stream. Running without audio.";
			}
		}

		if (audio_decoders_.size() == 1)
		{
			channel_layout = get_audio_channel_layout(
					audio_decoders_.at(0)->num_channels(),
					audio_decoders_.at(0)->ffmpeg_channel_layout(),
					custom_channel_order);
		}
		else if (audio_decoders_.size() > 1)
		{
			auto num_channels = cpplinq::from(audio_decoders_)
				.select(std::mem_fn(&audio_decoder::num_channels))
				.aggregate(0, std::plus<int>());
			auto ffmpeg_channel_layout = av_get_default_channel_layout(num_channels);

			channel_layout = get_audio_channel_layout(
					num_channels,
					ffmpeg_channel_layout,
					custom_channel_order);
		}

		if (!video_decoder_ && audio_decoders_.empty())
			CASPAR_THROW_EXCEPTION(averror_stream_not_found() << msg_info("No streams found"));

		muxer_.reset(new frame_muxer(framerate_, std::move(audio_input_pads), frame_factory, format_desc, channel_layout, filter, true));

		if (auto nb_frames = file_nb_frames())
		{
			out = std::min(out, nb_frames);
			input_.out(out);
		}
	}

	// frame_producer

	core::draw_frame receive_impl() override
	{
		return render_frame().first;
	}

	core::draw_frame last_frame() override
	{
		return core::draw_frame::still(last_frame_);
	}

	core::constraints& pixel_constraints() override
	{
		return constraints_;
	}

	double out_fps() const
	{
		auto out_framerate	= muxer_->out_framerate();
		auto fps			= static_cast<double>(out_framerate.numerator()) / static_cast<double>(out_framerate.denominator());

		return fps;
	}

	std::pair<core::draw_frame, uint32_t> render_frame()
	{
		frame_timer_.restart();

		for (int n = 0; n < 16 && frame_buffer_.size() < 2; ++n)
			try_decode_frame();

		graph_->set_value("frame-time", frame_timer_.elapsed() * out_fps() *0.5);

		if (frame_buffer_.empty())
		{
			if (input_.eof())
			{
				send_osc();
				return std::make_pair(last_frame(), -1);
			}
			else if (!is_url())
			{
				graph_->set_tag(diagnostics::tag_severity::WARNING, "underflow");
				send_osc();
				return std::make_pair(last_frame_, -1);
			}
			else
			{
				send_osc();
				return std::make_pair(last_frame_, -1);
			}
		}

		auto frame = frame_buffer_.front();
		frame_buffer_.pop();

		++frame_number_;
		file_frame_number_ = frame.second;

		graph_->set_text(print());

		last_frame_ = frame.first;

		send_osc();

		return frame;
	}

	bool is_url() const
	{
		return boost::contains(filename_, L"://");
	}

	void send_osc()
	{
		double fps = static_cast<double>(framerate_.numerator()) / static_cast<double>(framerate_.denominator());

		*monitor_subject_	<< core::monitor::message("/profiler/time")		% frame_timer_.elapsed() % (1.0/out_fps());

		*monitor_subject_	<< core::monitor::message("/file/time")			% (file_frame_number()/fps)
																			% (file_nb_frames()/fps)
							<< core::monitor::message("/file/frame")			% static_cast<int32_t>(file_frame_number())
																			% static_cast<int32_t>(file_nb_frames())
							<< core::monitor::message("/file/fps")			% fps
							<< core::monitor::message("/file/path")			% path_relative_to_media_
							<< core::monitor::message("/loop")				% input_.loop();
	}

	core::draw_frame render_specific_frame(uint32_t file_position)
	{
		// Some trial and error and undeterministic stuff here
		static const int NUM_RETRIES = 32;

		if (file_position > 0) // Assume frames are requested in sequential order,
			                   // therefore no seeking should be necessary for the first frame.
		{
			input_.seek(file_position > 1 ? file_position - 2: file_position).get();
			boost::this_thread::sleep_for(boost::chrono::milliseconds(40));
		}

		for (int i = 0; i < NUM_RETRIES; ++i)
		{
			boost::this_thread::sleep_for(boost::chrono::milliseconds(40));

			auto frame = render_frame();

			if (frame.second == std::numeric_limits<uint32_t>::max())
			{
				// Retry
				continue;
			}
			else if (frame.second == file_position + 1 || frame.second == file_position)
				return frame.first;
			else if (frame.second > file_position + 1)
			{
				CASPAR_LOG(trace) << print() << L" " << frame.second << L" received, wanted " << file_position + 1;
				int64_t adjusted_seek = file_position - (frame.second - file_position + 1);

				if (adjusted_seek > 1 && file_position > 0)
				{
					CASPAR_LOG(trace) << print() << L" adjusting to " << adjusted_seek;
					input_.seek(static_cast<uint32_t>(adjusted_seek) - 1).get();
					boost::this_thread::sleep_for(boost::chrono::milliseconds(40));
				}
				else
					return frame.first;
			}
		}

		CASPAR_LOG(trace) << print() << " Giving up finding frame at " << file_position;
		return core::draw_frame::empty();
	}

	uint32_t file_frame_number() const
	{
		return video_decoder_ ? video_decoder_->file_frame_number() : 0;
	}

	uint32_t nb_frames() const override
	{
		if (is_url() || input_.loop())
			return std::numeric_limits<uint32_t>::max();

		auto nb_frames = std::min(input_.out(), file_nb_frames());
		if (nb_frames >= input_.in())
			nb_frames -= input_.in();
		else
			nb_frames = 0;

		return muxer_->calc_nb_frames(nb_frames);
	}

	uint32_t file_nb_frames() const
	{
		return video_decoder_ ? video_decoder_->nb_frames() : 0;
	}

	std::future<std::wstring> call(const std::vector<std::wstring>& params) override
	{
		std::wstring result;

		std::wstring cmd = params.at(0);
		std::wstring value;
		if (params.size() > 1)
			value = params.at(1);

		if (boost::iequals(cmd, L"loop"))
		{
			if (!value.empty())
				input_.loop(boost::lexical_cast<bool>(value));
			result = boost::lexical_cast<std::wstring>(input_.loop());
		}
		else if (boost::iequals(cmd, L"in") || boost::iequals(cmd, L"start"))
		{
			if (!value.empty())
				input_.in(boost::lexical_cast<uint32_t>(value));
			result = boost::lexical_cast<std::wstring>(input_.in());
		}
		else if (boost::iequals(cmd, L"out"))
		{
			if (!value.empty())
				input_.out(boost::lexical_cast<uint32_t>(value));
			result = boost::lexical_cast<std::wstring>(input_.out());
		}
		else if (boost::iequals(cmd, L"length"))
		{
			if (!value.empty())
				input_.length(boost::lexical_cast<uint32_t>(value));
			result = boost::lexical_cast<std::wstring>(input_.length());
		}
		else if (boost::iequals(cmd, L"seek") && !value.empty())
		{
			auto nb_frames = file_nb_frames();

			int64_t seek;
			if (boost::iequals(value, L"rel"))
				seek = file_frame_number();
			else if (boost::iequals(value, L"in"))
				seek = input_.in();
			else if (boost::iequals(value, L"out"))
				seek = input_.out();
			else if (boost::iequals(value, L"end"))
				seek = nb_frames;
			else
				seek = boost::lexical_cast<int64_t>(value);

			if (params.size() > 2)
				seek += boost::lexical_cast<int64_t>(params.at(2));

			if (seek < 0)
				seek = 0;
			else if (seek >= nb_frames)
				seek = nb_frames - 1;

			input_.seek(static_cast<uint32_t>(seek));
		}
		else
			CASPAR_THROW_EXCEPTION(invalid_argument());

		return make_ready_future(std::move(result));
	}

	std::wstring print() const override
	{
		return L"ffmpeg[" + (is_url() ? filename_ : boost::filesystem::path(filename_).filename().wstring()) + L"|"
						  + print_mode() + L"|"
						  + boost::lexical_cast<std::wstring>(file_frame_number_) + L"/" + boost::lexical_cast<std::wstring>(file_nb_frames()) + L"]";
	}

	std::wstring name() const override
	{
		return L"ffmpeg";
	}

	boost::property_tree::wptree info() const override
	{
		boost::property_tree::wptree info;
		info.add(L"type",				L"ffmpeg-producer");
		info.add(L"filename",			filename_);
		info.add(L"width",				video_decoder_ ? video_decoder_->width() : 0);
		info.add(L"height",				video_decoder_ ? video_decoder_->height() : 0);
		info.add(L"progressive",		video_decoder_ ? video_decoder_->is_progressive() : false);
		info.add(L"fps",				static_cast<double>(framerate_.numerator()) / static_cast<double>(framerate_.denominator()));
		info.add(L"loop",				input_.loop());
		info.add(L"frame-number",		frame_number_);
		auto nb_frames2 = nb_frames();
		info.add(L"nb-frames",			nb_frames2 == std::numeric_limits<int64_t>::max() ? -1 : nb_frames2);
		info.add(L"file-frame-number",	file_frame_number_);
		info.add(L"file-nb-frames",		file_nb_frames());
		return info;
	}

	core::monitor::subject& monitor_output()
	{
		return *monitor_subject_;
	}

	// ffmpeg_producer

	std::wstring print_mode() const
	{
		return video_decoder_ ? ffmpeg::print_mode(
				video_decoder_->width(),
				video_decoder_->height(),
				static_cast<double>(framerate_.numerator()) / static_cast<double>(framerate_.denominator()),
				!video_decoder_->is_progressive()) : L"";
	}

	bool all_audio_decoders_ready() const
	{
		for (auto& audio_decoder : audio_decoders_)
			if (!audio_decoder->ready())
				return false;

		return true;
	}

	void try_decode_frame()
	{
		std::shared_ptr<AVPacket> pkt;

		for (int n = 0; n < 32 && ((video_decoder_ && !video_decoder_->ready()) || !all_audio_decoders_ready()) && input_.try_pop(pkt); ++n)
		{
			if (video_decoder_)
				video_decoder_->push(pkt);

			for (auto& audio_decoder : audio_decoders_)
				audio_decoder->push(pkt);
		}

		std::shared_ptr<AVFrame>									video;
		std::vector<std::shared_ptr<core::mutable_audio_buffer>>	audio;

		tbb::parallel_invoke(
		[&]
		{
			do
			{
				if (!muxer_->video_ready() && video_decoder_)
				{
					video = video_decoder_->poll();
					if (video)
						break;
				}
				else
					break;
			} while (!video_decoder_->empty());
		},
		[&]
		{
			if (!muxer_->audio_ready())
			{
				for (auto& audio_decoder : audio_decoders_)
				{
					auto audio_for_stream = audio_decoder->poll();

					if (audio_for_stream)
						audio.push_back(audio_for_stream);
				}
			}
		});

		muxer_->push(video);
		muxer_->push(audio);

		if (audio_decoders_.empty())
		{
			if (video == flush_video())
				muxer_->push({ flush_audio() });
			else if (!muxer_->audio_ready())
				muxer_->push({ empty_audio() });
		}

		if (!video_decoder_)
		{
			if (boost::count_if(audio, [](std::shared_ptr<core::mutable_audio_buffer> a) { return a == flush_audio(); }) > 0)
				muxer_->push(flush_video());
			else if (!muxer_->video_ready())
				muxer_->push(empty_video());
		}

		uint32_t file_frame_number = 0;
		file_frame_number = std::max(file_frame_number, video_decoder_ ? video_decoder_->file_frame_number() : 0);

		for (auto frame = muxer_->poll(); frame != core::draw_frame::empty(); frame = muxer_->poll())
			if (frame != core::draw_frame::empty())
				frame_buffer_.push(std::make_pair(frame, file_frame_number));
	}

	bool audio_only() const
	{
		return !video_decoder_;
	}

	boost::rational<int> get_out_framerate() const
	{
		return muxer_->out_framerate();
	}
};

spl::shared_ptr<core::frame_producer> create_producer(
		const core::frame_producer_dependencies& dependencies,
		const std::vector<std::wstring>& params)
{
	auto file_or_url	= params.at(0);

	if (!boost::contains(file_or_url, L"://"))
	{
		// File
		file_or_url = probe_stem(env::media_folder() + L"/" + file_or_url, false);
	}

	if (file_or_url.empty())
		return core::frame_producer::empty();

	constexpr auto uint32_max = std::numeric_limits<uint32_t>::max();

	auto loop					= contains_param(L"LOOP",		params);

	auto in						= get_param(L"SEEK",			params, static_cast<uint32_t>(0)); // compatibility
	in							= get_param(L"IN",				params, in);

	auto out					= get_param(L"LENGTH",			params, uint32_max);
	if (out < uint32_max - in)
		out += in;
	else
		out = uint32_max;
	out							= get_param(L"OUT",				params, out);

	auto filter_str				= get_param(L"FILTER",			params, L"");
	auto custom_channel_order	= get_param(L"CHANNEL_LAYOUT",	params, L"");

	boost::ireplace_all(filter_str, L"DEINTERLACE_BOB",	L"YADIF=1:-1");
	boost::ireplace_all(filter_str, L"DEINTERLACE_LQ",	L"SEPARATEFIELDS");
	boost::ireplace_all(filter_str, L"DEINTERLACE",		L"YADIF=0:-1");

	ffmpeg_options vid_params;
	bool haveFFMPEGStartIndicator = false;
	for (size_t i = 0; i < params.size() - 1; ++i)
	{
		if (!haveFFMPEGStartIndicator && params[i] == L"--")
		{
			haveFFMPEGStartIndicator = true;
			continue;
		}
		if (haveFFMPEGStartIndicator)
		{
			auto name = u8(params.at(i++)).substr(1);
			auto value = u8(params.at(i));
			vid_params.push_back(std::make_pair(name, value));
		}
	}

	auto producer = spl::make_shared<ffmpeg_producer>(
			dependencies.frame_factory,
			dependencies.format_desc,
			file_or_url,
			filter_str,
			loop,
			in,
			out,
			custom_channel_order,
			vid_params);

	if (producer->audio_only())
		return core::create_destroy_proxy(producer);

	auto get_source_framerate	= [=] { return producer->get_out_framerate(); };
	auto target_framerate		= dependencies.format_desc.framerate;

	return core::create_destroy_proxy(core::create_framerate_producer(
			producer,
			get_source_framerate,
			target_framerate,
			dependencies.format_desc.field_mode,
			dependencies.format_desc.audio_cadence));
}

}}
