#include "../StdAfx.h"

#include "frame_muxer.h"

#include "filter/filter.h"

#include "util.h"
#include "display_mode.h"

#include <core/producer/frame_producer.h>
#include <core/producer/frame/basic_frame.h>
#include <core/producer/frame/frame_factory.h>
#include <core/mixer/write_frame.h>

#include <common/concurrency/governor.h>
#include <common/env.h>
#include <common/exception/exceptions.h>
#include <common/log/log.h>

#include <boost/range/algorithm_ext/push_back.hpp>

#if defined(_MSC_VER)
#pragma warning (push)
#pragma warning (disable : 4244)
#endif
extern "C" 
{
	#define __STDC_CONSTANT_MACROS
	#define __STDC_LIMIT_MACROS
	#include <libavcodec/avcodec.h>
	#include <libavformat/avformat.h>
}
#if defined(_MSC_VER)
#pragma warning (pop)
#endif

#include <agents.h>

using namespace Concurrency;

namespace caspar { namespace ffmpeg {
	
struct frame_muxer2::implementation : public Concurrency::agent, boost::noncopyable
{		
	frame_muxer2::video_source_t* video_source_;
	frame_muxer2::audio_source_t* audio_source_;

	ITarget<frame_muxer2::target_element_t>&			target_;
	mutable single_assignment<display_mode::type>		display_mode_;
	const double										in_fps_;
	const core::video_format_desc						format_desc_;
	const bool											auto_transcode_;
	
	mutable single_assignment<safe_ptr<filter>>			filter_;
	const safe_ptr<core::frame_factory>					frame_factory_;
			
	core::audio_buffer									audio_data_;
	
	std::wstring										filter_str_;

	governor											governor_;
	tbb::atomic<bool>									is_running_;
	
	implementation(frame_muxer2::video_source_t* video_source,
				   frame_muxer2::audio_source_t* audio_source,
				   frame_muxer2::target_t& target,
				   double in_fps, 
				   const safe_ptr<core::frame_factory>& frame_factory,
				   const std::wstring& filter)
		: video_source_(video_source)
		, audio_source_(audio_source)
		, target_(target)
		, in_fps_(in_fps)
		, format_desc_(frame_factory->get_video_format_desc())
		, auto_transcode_(env::properties().get("configuration.producers.auto-transcode", false))
		, frame_factory_(frame_factory)
		, governor_(1)
	{		
		is_running_ = true;
		start();
	}

	~implementation()
	{
		agent::wait(this);
	}
				
	std::shared_ptr<core::write_frame> receive_video()
	{	
		if(!video_source_)
			return make_safe<core::write_frame>(this);	

		auto video = filter_.has_value() ? filter_.value()->poll() : nullptr;
		if(video)		
			return make_write_frame(this, make_safe_ptr(video), frame_factory_, 0);		
		
		video = receive(video_source_);
		
		if(video == loop_video())
			return receive_video();

		if(video == eof_video())
		{
			is_running_ = false;
			return nullptr;
		}

		if(!display_mode_.has_value())
			initialize_display_mode(*video);
			
		filter_.value()->push(std::move(video));

		return receive_video();
	}
	
	std::shared_ptr<core::audio_buffer> receive_audio()
	{		
		if(!audio_source_)
			return make_safe<core::audio_buffer>(format_desc_.audio_samples_per_frame, 0);
					
		if(audio_data_.size() >= format_desc_.audio_samples_per_frame)
		{
			auto begin = audio_data_.begin(); 
			auto end   = begin + format_desc_.audio_samples_per_frame;
			auto audio = make_safe<core::audio_buffer>(begin, end);
			audio_data_.erase(begin, end);
			return audio;
		}
				
		auto audio = receive(audio_source_);

		if(audio == loop_audio())
		{
			if(!audio_data_.empty())
			{
				CASPAR_LOG(info) << L"[frame_muxer] Truncating audio: " << audio_data_.size();
				audio_data_.clear();
			}
		}

		if(audio == eof_audio())
		{
			is_running_ = false;
			return nullptr;
		}

		audio_data_.insert(audio_data_.end(), audio->begin(), audio->end());	

		return receive_audio();
	}
			
	virtual void run()
	{
		try
		{
			win32_exception::install_handler();
			while(is_running_)
			{	
				auto ticket = governor_.acquire();

				auto video = receive_video();
				if(!video)
					break;

				auto audio = receive_audio();

				if(!audio)
					break;

				video->audio_data() = std::move(*audio);
				
				switch(display_mode_.value())
				{
				case display_mode::simple:			
				case display_mode::deinterlace:
				case display_mode::deinterlace_bob:
					{
						send(target_, frame_muxer2::target_element_t(video, ticket));

						break;
					}
				case display_mode::duplicate:					
					{			
						auto video2 = make_safe<core::write_frame>(*video);	
						auto audio2 = receive_audio();
						
						send(target_, frame_muxer2::target_element_t(video, ticket));
						if(audio2)
						{
							video2->audio_data() = std::move(*receive_audio());					
							send(target_, frame_muxer2::target_element_t(video2, ticket));
						}

						break;
					}
				case display_mode::half:						
					{								
						send(target_, frame_muxer2::target_element_t(video, ticket));
						receive_video();

						break;
					}
				case display_mode::deinterlace_bob_reinterlace:
				case display_mode::interlace:					
					{					
						std::shared_ptr<core::basic_frame> video2 = receive_video();
						if(!video2)
							video2 = core::basic_frame::empty();
												
						auto frame = core::basic_frame::interlace(make_safe_ptr(video), make_safe_ptr(video2), format_desc_.field_mode);	
						send(target_, frame_muxer2::target_element_t(frame, ticket));

						break;
					}
				default:	
					BOOST_THROW_EXCEPTION(invalid_operation() << msg_info("invalid display-mode"));
				}
			}	
		}
		catch(...)
		{
			CASPAR_LOG_CURRENT_EXCEPTION();
		}
		
		send(target_, frame_muxer2::target_element_t(core::basic_frame::eof(), ticket_t()));

		done();
	}

	void initialize_display_mode(AVFrame& frame)
	{
		auto display_mode = display_mode::invalid;

		if(auto_transcode_)
		{
			auto mode = get_mode(frame);
			auto fps  = in_fps_;

			if(is_deinterlacing(filter_str_))
				mode = core::field_mode::progressive;

			if(is_double_rate(filter_str_))
				fps *= 2;

			display_mode = get_display_mode(mode, fps, format_desc_.field_mode, format_desc_.fps);
			
			if(display_mode == display_mode::simple && mode != core::field_mode::progressive && format_desc_.field_mode != core::field_mode::progressive && frame.height != static_cast<int>(format_desc_.height))
				display_mode = display_mode::deinterlace_bob_reinterlace; // The frame will most likely be scaled, we need to deinterlace->reinterlace	
				
			if(display_mode == display_mode::deinterlace)
				append_filter(filter_str_, L"YADIF=0:-1");
			else if(display_mode == display_mode::deinterlace_bob || display_mode == display_mode::deinterlace_bob_reinterlace)
				append_filter(filter_str_, L"YADIF=1:-1");
		}
		else
			display_mode = display_mode::simple;

		if(display_mode == display_mode::invalid)
		{
			CASPAR_LOG(warning) << L"[frame_muxer] Failed to detect display-mode.";
			display_mode = display_mode::simple;
		}
			
		send(filter_, make_safe<filter>(filter_str_));

		CASPAR_LOG(info) << "[frame_muxer] " << display_mode::print(display_mode);

		send(display_mode_, display_mode);
	}
					
	int64_t calc_nb_frames(int64_t nb_frames) const
	{
		switch(display_mode_.value()) // Take into account transformation in run.
		{
		case display_mode::deinterlace_bob_reinterlace:
		case display_mode::interlace:	
		case display_mode::half:
			nb_frames /= 2;
			break;
		case display_mode::duplicate:
			nb_frames *= 2;
			break;
		}

		if(is_double_rate(widen(filter_.value()->filter_str()))) // Take into account transformations in filter.
			nb_frames *= 2;

		return nb_frames;
	}
};

frame_muxer2::frame_muxer2(video_source_t* video_source, 
						   audio_source_t* audio_source,
						   target_t& target,
						   double in_fps, 
						   const safe_ptr<core::frame_factory>& frame_factory,
						   const std::wstring& filter)
	: impl_(new implementation(video_source, audio_source, target, in_fps, frame_factory, filter))
{
}

int64_t frame_muxer2::calc_nb_frames(int64_t nb_frames) const
{
	return impl_->calc_nb_frames(nb_frames);
}

}}