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
* Author: Nicklas P Andersson
*/

#include "../StdAfx.h"

#if defined(_MSC_VER)
#pragma warning (push, 1) // TODO: Legacy code, just disable warnings
#endif

#include "AMCPCommandsImpl.h"

#include "amcp_command_repository.h"
#include "AMCPCommandQueue.h"

#include <common/env.h>

#include <common/log.h>
#include <common/param.h>
#include <common/os/filesystem.h>
#include <common/base64.h>
#include <common/filesystem.h>

#include <core/producer/cg_proxy.h>
#include <core/producer/frame_producer.h>
#include <core/video_format.h>
#include <core/producer/transition/transition_producer.h>
#include <core/frame/frame_transform.h>
#include <core/producer/text/text_producer.h>
#include <core/producer/stage.h>
#include <core/producer/layer.h>
#include <core/mixer/mixer.h>
#include <core/consumer/output.h>
#include <core/diagnostics/call_context.h>
#include <core/diagnostics/osd_graph.h>

#include <algorithm>
#include <locale>
#include <fstream>
#include <memory>
#include <cctype>
#include <future>

#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/regex.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <boost/locale.hpp>
#include <boost/range/adaptor/transformed.hpp>
#include <boost/range/algorithm/copy.hpp>
#include <boost/archive/iterators/base64_from_binary.hpp>
#include <boost/archive/iterators/insert_linebreaks.hpp>
#include <boost/archive/iterators/transform_width.hpp>

#include <tbb/concurrent_unordered_map.h>

/* Return codes

102 [action]			Information that [action] has happened
101 [action]			Information that [action] has happened plus one row of data

202 [command] OK		[command] has been executed
201 [command] OK		[command] has been executed, plus one row of data
200 [command] OK		[command] has been executed, plus multiple lines of data. ends with an empty line

400 ERROR				the command could not be understood
401 [command] ERROR		invalid/missing channel
402 [command] ERROR		parameter missing
403 [command] ERROR		invalid parameter
404 [command] ERROR		file not found

500 FAILED				internal error
501 [command] FAILED	internal error
502 [command] FAILED	could not read file
503 [command] FAILED	access denied

600 [command] FAILED	[command] not implemented
*/

namespace caspar { namespace protocol { namespace amcp {

using namespace core;

std::wstring read_file_base64(const boost::filesystem::path& file)
{
	using namespace boost::archive::iterators;

	boost::filesystem::ifstream filestream(file, std::ios::binary);

	if (!filestream)
		return L"";

	auto length = boost::filesystem::file_size(file);
	std::vector<char> bytes;
	bytes.resize(length);
	filestream.read(bytes.data(), length);

	std::string result(to_base64(bytes.data(), length));
	return std::wstring(result.begin(), result.end());
}

std::wstring read_utf8_file(const boost::filesystem::path& file)
{
	std::wstringstream result;
	boost::filesystem::wifstream filestream(file);

	if (filestream)
	{
		// Consume BOM first
		filestream.get();
		// read all data
		result << filestream.rdbuf();
	}

	return result.str();
}

std::wstring read_latin1_file(const boost::filesystem::path& file)
{
	boost::locale::generator gen;
	gen.locale_cache_enabled(true);
	gen.categories(boost::locale::codepage_facet);

	std::stringstream result_stream;
	boost::filesystem::ifstream filestream(file);
	filestream.imbue(gen("en_US.ISO8859-1"));

	if (filestream)
	{
		// read all data
		result_stream << filestream.rdbuf();
	}

	std::string result = result_stream.str();
	std::wstring widened_result;

	// The first 255 codepoints in unicode is the same as in latin1
	boost::copy(
		result | boost::adaptors::transformed(
				[](char c) { return static_cast<unsigned char>(c); }),
		std::back_inserter(widened_result));

	return widened_result;
}

std::wstring read_file(const boost::filesystem::path& file)
{
	static const uint8_t BOM[] = {0xef, 0xbb, 0xbf};

	if (!boost::filesystem::exists(file))
	{
		return L"";
	}

	if (boost::filesystem::file_size(file) >= 3)
	{
		boost::filesystem::ifstream bom_stream(file);

		char header[3];
		bom_stream.read(header, 3);
		bom_stream.close();

		if (std::memcmp(BOM, header, 3) == 0)
			return read_utf8_file(file);
	}

	return read_latin1_file(file);
}

std::wstring get_sub_directory(const std::wstring& base_folder, const std::wstring& sub_directory)
{
	if (sub_directory.empty())
		return base_folder;

	auto found = find_case_insensitive(base_folder + L"/" + sub_directory);

	if (!found)
		CASPAR_THROW_EXCEPTION(file_not_found() << msg_info(L"Sub directory " + sub_directory + L" not found."));

	return *found;
}

std::vector<spl::shared_ptr<core::video_channel>> get_channels(const command_context& ctx)
{
    std::vector<spl::shared_ptr<core::video_channel>> result;
    for (auto& cc : ctx.channels) {
        result.push_back(spl::make_shared_ptr(cc.channel));
    }
    return result;
}

core::frame_producer_dependencies get_producer_dependencies(const std::shared_ptr<core::video_channel>& channel, const command_context& ctx)
{
	return core::frame_producer_dependencies(
			channel->frame_factory(),
			get_channels(ctx),
			channel->video_format_desc(),
			ctx.producer_registry,
			ctx.cg_registry);
}

// Basic Commands

std::wstring loadbg_command(command_context& ctx)
{
	transition_info transitionInfo;

	// TRANSITION

	std::wstring message;
	for (size_t n = 0; n < ctx.parameters.size(); ++n)
		message += boost::to_upper_copy(ctx.parameters[n]) + L" ";

	static const boost::wregex expr(LR"(.*(?<TRANSITION>CUT|PUSH|SLIDE|WIPE|MIX)\s*(?<DURATION>\d+)\s*(?<TWEEN>(LINEAR)|(EASE[^\s]*))?\s*(?<DIRECTION>FROMLEFT|FROMRIGHT|LEFT|RIGHT)?.*)");
	boost::wsmatch what;
	if (boost::regex_match(message, what, expr))
	{
		auto transition = what["TRANSITION"].str();
		transitionInfo.duration = boost::lexical_cast<size_t>(what["DURATION"].str());
		auto direction = what["DIRECTION"].matched ? what["DIRECTION"].str() : L"";
		auto tween = what["TWEEN"].matched ? what["TWEEN"].str() : L"";
		transitionInfo.tweener = tween;

		if (transition == L"CUT")
			transitionInfo.type = transition_type::cut;
		else if (transition == L"MIX")
			transitionInfo.type = transition_type::mix;
		else if (transition == L"PUSH")
			transitionInfo.type = transition_type::push;
		else if (transition == L"SLIDE")
			transitionInfo.type = transition_type::slide;
		else if (transition == L"WIPE")
			transitionInfo.type = transition_type::wipe;

		if (direction == L"FROMLEFT")
			transitionInfo.direction = transition_direction::from_left;
		else if (direction == L"FROMRIGHT")
			transitionInfo.direction = transition_direction::from_right;
		else if (direction == L"LEFT")
			transitionInfo.direction = transition_direction::from_right;
		else if (direction == L"RIGHT")
			transitionInfo.direction = transition_direction::from_left;
	}

	//Perform loading of the clip
	core::diagnostics::scoped_call_context save;
	core::diagnostics::call_context::for_thread().video_channel = ctx.channel_index + 1;
	core::diagnostics::call_context::for_thread().layer = ctx.layer_index();

	auto channel = ctx.channel.channel;
	auto pFP = ctx.producer_registry->create_producer(get_producer_dependencies(channel, ctx), ctx.parameters);

	if (pFP == frame_producer::empty())
		CASPAR_THROW_EXCEPTION(file_not_found() << msg_info(ctx.parameters.size() > 0 ? ctx.parameters[0] : L""));

	bool auto_play = contains_param(L"AUTO", ctx.parameters);

	auto pFP2 = create_transition_producer(channel->video_format_desc().field_mode, pFP, transitionInfo);
	if (auto_play)
		channel->stage().load(ctx.layer_index(), pFP2, false, transitionInfo.duration); // TODO: LOOP
	else
		channel->stage().load(ctx.layer_index(), pFP2, false); // TODO: LOOP

	return L"202 LOADBG OK\r\n";
}

std::wstring load_command(command_context& ctx)
{
	core::diagnostics::scoped_call_context save;
	core::diagnostics::call_context::for_thread().video_channel = ctx.channel_index + 1;
	core::diagnostics::call_context::for_thread().layer = ctx.layer_index();
	auto pFP = ctx.producer_registry->create_producer(get_producer_dependencies(ctx.channel.channel, ctx), ctx.parameters);
	ctx.channel.channel->stage().load(ctx.layer_index(), pFP, true);

	return L"202 LOAD OK\r\n";
}

std::wstring play_command(command_context& ctx)
{
	if (!ctx.parameters.empty())
		loadbg_command(ctx);

	ctx.channel.channel->stage().play(ctx.layer_index());

	return L"202 PLAY OK\r\n";
}

std::wstring pause_command(command_context& ctx)
{
	ctx.channel.channel->stage().pause(ctx.layer_index());
	return L"202 PAUSE OK\r\n";
}

std::wstring resume_command(command_context& ctx)
{
	ctx.channel.channel->stage().resume(ctx.layer_index());
	return L"202 RESUME OK\r\n";
}

std::wstring stop_command(command_context& ctx)
{
	ctx.channel.channel->stage().stop(ctx.layer_index());
	return L"202 STOP OK\r\n";
}

std::wstring clear_command(command_context& ctx)
{
	int index = ctx.layer_index(std::numeric_limits<int>::min());
	if (index != std::numeric_limits<int>::min())
		ctx.channel.channel->stage().clear(index);
	else
		ctx.channel.channel->stage().clear();

	return L"202 CLEAR OK\r\n";
}

std::wstring call_command(command_context& ctx)
{
	auto result = ctx.channel.channel->stage().call(ctx.layer_index(), ctx.parameters).get();

	// TODO: because of std::async deferred timed waiting does not work

	/*auto wait_res = result.wait_for(std::chrono::seconds(2));
	if (wait_res == std::future_status::timeout)
	CASPAR_THROW_EXCEPTION(timed_out());*/

	std::wstringstream replyString;
	if (result.empty())
		replyString << L"202 CALL OK\r\n";
	else
		replyString << L"201 CALL OK\r\n" << result << L"\r\n";

	return replyString.str();
}

std::wstring swap_command(command_context& ctx)
{
	bool swap_transforms = ctx.parameters.size() > 1 && boost::iequals(ctx.parameters.at(1), L"TRANSFORMS");

	if (ctx.layer_index(-1) != -1)
	{
		std::vector<std::string> strs;
		boost::split(strs, ctx.parameters[0], boost::is_any_of("-"));

		auto ch1 = ctx.channel.channel;
		auto ch2 = ctx.channels.at(boost::lexical_cast<int>(strs.at(0)) - 1);

		int l1 = ctx.layer_index();
		int l2 = boost::lexical_cast<int>(strs.at(1));

		ch1->stage().swap_layer(l1, l2, ch2.channel->stage(), swap_transforms);
	}
	else
	{
		auto ch1 = ctx.channel.channel;
		auto ch2 = ctx.channels.at(boost::lexical_cast<int>(ctx.parameters[0]) - 1);
		ch1->stage().swap_layers(ch2.channel->stage(), swap_transforms);
	}

	return L"202 SWAP OK\r\n";
}

std::wstring add_command(command_context& ctx)
{
	replace_placeholders(
			L"<CLIENT_IP_ADDRESS>",
			ctx.client->address(),
			ctx.parameters);

	core::diagnostics::scoped_call_context save;
	core::diagnostics::call_context::for_thread().video_channel = ctx.channel_index + 1;

	auto consumer = ctx.consumer_registry->create_consumer(ctx.parameters, &ctx.channel.channel->stage(), get_channels(ctx));
	ctx.channel.channel->output().add(ctx.layer_index(consumer->index()), consumer);

	return L"202 ADD OK\r\n";
}

std::wstring remove_command(command_context& ctx)
{
	auto index = ctx.layer_index(std::numeric_limits<int>::min());

	if (index == std::numeric_limits<int>::min())
	{
		replace_placeholders(
				L"<CLIENT_IP_ADDRESS>",
				ctx.client->address(),
				ctx.parameters);

		index = ctx.consumer_registry->create_consumer(ctx.parameters, &ctx.channel.channel->stage(), get_channels(ctx))->index();
	}

	ctx.channel.channel->output().remove(index);

	return L"202 REMOVE OK\r\n";
}

std::wstring print_command(command_context& ctx)
{
	ctx.channel.channel->output().add(ctx.consumer_registry->create_consumer({ L"IMAGE" }, &ctx.channel.channel->stage(), get_channels(ctx)));

	return L"202 PRINT OK\r\n";
}

std::wstring log_level_command(command_context& ctx)
{
	log::set_log_level(ctx.parameters.at(0));

	return L"202 LOG OK\r\n";
}

std::wstring log_category_command(command_context& ctx)
{
	log::set_log_category(ctx.parameters.at(0), ctx.parameters.at(1) == L"1");

	return L"202 LOG OK\r\n";
}

std::wstring set_command(command_context& ctx)
{
	std::wstring name = boost::to_upper_copy(ctx.parameters[0]);
	std::wstring value = boost::to_upper_copy(ctx.parameters[1]);

	if (name == L"MODE")
	{
		auto format_desc = core::video_format_desc(value);
		if (format_desc.format != core::video_format::invalid)
		{
			ctx.channel.channel->video_format_desc(format_desc);
			return L"202 SET MODE OK\r\n";
		}

		CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"Invalid video mode"));
	}

	CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"Invalid channel variable"));
}

std::wstring data_store_command(command_context& ctx)
{
	std::wstring filename = env::data_folder();
	filename.append(ctx.parameters[0]);
	filename.append(L".ftd");

	auto data_path = boost::filesystem::path(filename).parent_path().wstring();
	auto found_data_path = find_case_insensitive(data_path);

	if (found_data_path)
		data_path = *found_data_path;

	if (!boost::filesystem::exists(data_path))
		boost::filesystem::create_directories(data_path);

	auto found_filename = find_case_insensitive(filename);

	if (found_filename)
		filename = *found_filename; // Overwrite case insensitive.

	boost::filesystem::wofstream datafile(filename);
	if (!datafile)
		CASPAR_THROW_EXCEPTION(caspar_exception() << msg_info(L"Could not open file " + filename));

	datafile << static_cast<wchar_t>(65279); // UTF-8 BOM character
	datafile << ctx.parameters[1] << std::flush;
	datafile.close();

	return L"202 DATA STORE OK\r\n";
}

std::wstring data_retrieve_command(command_context& ctx)
{
	std::wstring filename = env::data_folder();
	filename.append(ctx.parameters[0]);
	filename.append(L".ftd");

	std::wstring file_contents;

	auto found_file = find_case_insensitive(filename);

	if (found_file)
		file_contents = read_file(boost::filesystem::path(*found_file));

	if (file_contents.empty())
		CASPAR_THROW_EXCEPTION(file_not_found() << msg_info(filename + L" not found"));

	std::wstringstream reply;
	reply << L"201 DATA RETRIEVE OK\r\n";

	std::wstringstream file_contents_stream(file_contents);
	std::wstring line;

	bool firstLine = true;
	while (std::getline(file_contents_stream, line))
	{
		if (firstLine)
			firstLine = false;
		else
			reply << "\n";

		reply << line;
	}

	reply << "\r\n";
	return reply.str();
}

std::wstring data_list_command(command_context& ctx)
{
	std::wstring sub_directory;

	if (!ctx.parameters.empty())
		sub_directory = ctx.parameters.at(0);

	std::wstringstream replyString;
	replyString << L"200 DATA LIST OK\r\n";

	for (boost::filesystem::recursive_directory_iterator itr(get_sub_directory(env::data_folder(), sub_directory)), end; itr != end; ++itr)
	{
		if (boost::filesystem::is_regular_file(itr->path()))
		{
			if (!boost::iequals(itr->path().extension().wstring(), L".ftd"))
				continue;

			auto relativePath = get_relative_without_extension(itr->path(), env::data_folder());
			auto str = relativePath.generic_wstring();

			if (str[0] == L'\\' || str[0] == L'/')
				str = std::wstring(str.begin() + 1, str.end());

			replyString << str << L"\r\n";
		}
	}

	replyString << L"\r\n";

	return boost::to_upper_copy(replyString.str());
}

std::wstring data_remove_command(command_context& ctx)
{
	std::wstring filename = env::data_folder();
	filename.append(ctx.parameters[0]);
	filename.append(L".ftd");

	if (!boost::filesystem::exists(filename))
		CASPAR_THROW_EXCEPTION(file_not_found() << msg_info(filename + L" not found"));

	if (!boost::filesystem::remove(filename))
		CASPAR_THROW_EXCEPTION(caspar_exception() << msg_info(filename + L" could not be removed"));

	return L"202 DATA REMOVE OK\r\n";
}

// Template Graphics Commands

std::wstring cg_add_command(command_context& ctx)
{
	//CG 1 ADD 0 "template_folder/templatename" [STARTLABEL] 0/1 [DATA]

	int layer = boost::lexical_cast<int>(ctx.parameters.at(0));
	std::wstring label;		//_parameters[2]
	bool bDoStart = false;		//_parameters[2] alt. _parameters[3]
	unsigned int dataIndex = 3;

	if (ctx.parameters.at(2).length() > 1)
	{	//read label
		label = ctx.parameters.at(2);
		++dataIndex;

		if (ctx.parameters.at(3).length() > 0)	//read play-on-load-flag
			bDoStart = (ctx.parameters.at(3).at(0) == L'1') ? true : false;
	}
	else
	{	//read play-on-load-flag
		bDoStart = (ctx.parameters.at(2).at(0) == L'1') ? true : false;
	}

	const wchar_t* pDataString = 0;
	std::wstring dataFromFile;
	if (ctx.parameters.size() > dataIndex)
	{	//read data
		const std::wstring& dataString = ctx.parameters.at(dataIndex);

		if (dataString.at(0) == L'<' || dataString.at(0) == L'{') //the data is XML or Json
			pDataString = dataString.c_str();
		else
		{
			//The data is not an XML-string, it must be a filename
			std::wstring filename = env::data_folder();
			filename.append(dataString);
			filename.append(L".ftd");

			auto found_file = find_case_insensitive(filename);

			if (found_file)
			{
				dataFromFile = read_file(boost::filesystem::path(*found_file));
				pDataString = dataFromFile.c_str();
			}
		}
	}

	auto filename = ctx.parameters.at(1);
	auto proxy = ctx.cg_registry->get_or_create_proxy(
		spl::make_shared_ptr(ctx.channel.channel),
		get_producer_dependencies(ctx.channel.channel, ctx),
		ctx.layer_index(core::cg_proxy::DEFAULT_LAYER),
		filename);

	if (proxy == core::cg_proxy::empty())
		CASPAR_THROW_EXCEPTION(file_not_found() << msg_info(L"Could not find template " + filename));
	else
		proxy->add(layer, filename, bDoStart, label, (pDataString != 0) ? pDataString : L"");

	return L"202 CG OK\r\n";
}

std::wstring cg_play_command(command_context& ctx)
{
	int layer = boost::lexical_cast<int>(ctx.parameters.at(0));
	ctx.cg_registry->get_proxy(spl::make_shared_ptr(ctx.channel.channel), ctx.layer_index(core::cg_proxy::DEFAULT_LAYER))->play(layer);

	return L"202 CG OK\r\n";
}

spl::shared_ptr<core::cg_proxy> get_expected_cg_proxy(command_context& ctx)
{
	auto proxy = ctx.cg_registry->get_proxy(spl::make_shared_ptr(ctx.channel.channel), ctx.layer_index(core::cg_proxy::DEFAULT_LAYER));

	if (proxy == cg_proxy::empty())
		CASPAR_THROW_EXCEPTION(expected_user_error() << msg_info(L"No CG proxy running on layer"));

	return proxy;
}

std::wstring cg_stop_command(command_context& ctx)
{
	int layer = boost::lexical_cast<int>(ctx.parameters.at(0));
	get_expected_cg_proxy(ctx)->stop(layer, 0);

	return L"202 CG OK\r\n";
}

std::wstring cg_next_command(command_context& ctx)
{
	int layer = boost::lexical_cast<int>(ctx.parameters.at(0));
	get_expected_cg_proxy(ctx)->next(layer);

	return L"202 CG OK\r\n";
}

std::wstring cg_remove_command(command_context& ctx)
{
	int layer = boost::lexical_cast<int>(ctx.parameters.at(0));
	get_expected_cg_proxy(ctx)->remove(layer);

	return L"202 CG OK\r\n";
}

std::wstring cg_clear_command(command_context& ctx)
{
	ctx.channel.channel->stage().clear(ctx.layer_index(core::cg_proxy::DEFAULT_LAYER));

	return L"202 CG OK\r\n";
}

std::wstring cg_update_command(command_context& ctx)
{
	int layer = boost::lexical_cast<int>(ctx.parameters.at(0));

	std::wstring dataString = ctx.parameters.at(1);
	if (dataString.at(0) != L'<' && dataString.at(0) != L'{')
	{
		//The data is not XML or Json, it must be a filename
		std::wstring filename = env::data_folder();
		filename.append(dataString);
		filename.append(L".ftd");

		dataString = read_file(boost::filesystem::path(filename));
	}

	get_expected_cg_proxy(ctx)->update(layer, dataString);

	return L"202 CG OK\r\n";
}

std::wstring cg_invoke_command(command_context& ctx)
{
	std::wstringstream replyString;
	replyString << L"201 CG OK\r\n";
	int layer = boost::lexical_cast<int>(ctx.parameters.at(0));
	auto result = get_expected_cg_proxy(ctx)->invoke(layer, ctx.parameters.at(1));
	replyString << result << L"\r\n";

	return replyString.str();
}

std::wstring cg_info_command(command_context& ctx)
{
	std::wstringstream replyString;
	replyString << L"201 CG OK\r\n";

	if (ctx.parameters.empty())
	{
		auto info = get_expected_cg_proxy(ctx)->template_host_info();
		replyString << info << L"\r\n";
	}
	else
	{
		int layer = boost::lexical_cast<int>(ctx.parameters.at(0));
		auto desc = get_expected_cg_proxy(ctx)->description(layer);

		replyString << desc << L"\r\n";
	}

	return replyString.str();
}

// Mixer Commands

core::frame_transform get_current_transform(command_context& ctx)
{
	return ctx.channel.channel->stage().get_current_transform(ctx.layer_index()).get();
}

template<typename Func>
std::wstring reply_value(command_context& ctx, const Func& extractor)
{
	auto value = extractor(get_current_transform(ctx));

	return L"201 MIXER OK\r\n" + boost::lexical_cast<std::wstring>(value)+L"\r\n";
}

class transforms_applier
{
	static tbb::concurrent_unordered_map<int, std::vector<stage::transform_tuple_t>> deferred_transforms_;

	std::vector<stage::transform_tuple_t>	transforms_;
	command_context&						ctx_;
	bool									defer_;
public:
	transforms_applier(command_context& ctx)
		: ctx_(ctx)
	{
		defer_ = !ctx.parameters.empty() && boost::iequals(ctx.parameters.back(), L"DEFER");

		if (defer_)
			ctx.parameters.pop_back();
	}

	void add(stage::transform_tuple_t&& transform)
	{
		transforms_.push_back(std::move(transform));
	}

	void commit_deferred()
	{
		auto& transforms = deferred_transforms_[ctx_.channel_index];
		ctx_.channel.channel->stage().apply_transforms(transforms).get();
		transforms.clear();
	}

	void apply()
	{
		if (defer_)
		{
			auto& defer_tranforms = deferred_transforms_[ctx_.channel_index];
			defer_tranforms.insert(defer_tranforms.end(), transforms_.begin(), transforms_.end());
		}
		else
			ctx_.channel.channel->stage().apply_transforms(transforms_);
	}
};
tbb::concurrent_unordered_map<int, std::vector<stage::transform_tuple_t>> transforms_applier::deferred_transforms_;

std::wstring mixer_keyer_command(command_context& ctx)
{
	if (ctx.parameters.empty())
		return reply_value(ctx, [](const frame_transform& t) { return t.image_transform.is_key ? 1 : 0; });

	transforms_applier transforms(ctx);
	bool value = boost::lexical_cast<int>(ctx.parameters.at(0));
	transforms.add(stage::transform_tuple_t(ctx.layer_index(), [=](frame_transform transform) -> frame_transform
	{
		transform.image_transform.is_key = value;
		return transform;
	}, 0, tweener(L"linear")));
	transforms.apply();

	return L"202 MIXER OK\r\n";
}

std::wstring ANIMATION_SYNTAX = L" {[duration:int] {[tween:string]|linear}|0 linear}}";

std::wstring mixer_chroma_command(command_context& ctx)
{
	if (ctx.parameters.empty())
	{
		auto chroma = get_current_transform(ctx).image_transform.chroma;
		return L"201 MIXER OK\r\n"
			+ std::wstring(chroma.enable ? L"1 " : L"0 ")
			+ boost::lexical_cast<std::wstring>(chroma.target_hue) + L" "
			+ boost::lexical_cast<std::wstring>(chroma.hue_width) + L" "
			+ boost::lexical_cast<std::wstring>(chroma.min_saturation) + L" "
			+ boost::lexical_cast<std::wstring>(chroma.min_brightness) + L" "
			+ boost::lexical_cast<std::wstring>(chroma.softness) + L" "
			+ boost::lexical_cast<std::wstring>(chroma.spill_suppress) + L" "
			+ boost::lexical_cast<std::wstring>(chroma.spill_suppress_saturation) + L" "
			+ std::wstring(chroma.show_mask ? L"1" : L"0") + L"\r\n";
	}

	transforms_applier transforms(ctx);
	core::chroma chroma;

	int duration;
	std::wstring tween;

	auto legacy_mode = core::get_chroma_mode(ctx.parameters.at(0));

	if (legacy_mode)
	{

		duration = ctx.parameters.size() > 4 ? boost::lexical_cast<int>(ctx.parameters.at(4)) : 0;
		tween = ctx.parameters.size() > 5 ? ctx.parameters.at(5) : L"linear";

		if (*legacy_mode == chroma::legacy_type::none)
		{
			chroma.enable = false;
		}
		else
		{
			chroma.enable						= true;
			chroma.hue_width					= 0.5 - boost::lexical_cast<double>(ctx.parameters.at(1)) * 0.5;
			chroma.min_brightness				= boost::lexical_cast<double>(ctx.parameters.at(1));
			chroma.min_saturation				= boost::lexical_cast<double>(ctx.parameters.at(1));
			chroma.softness						= boost::lexical_cast<double>(ctx.parameters.at(2)) - boost::lexical_cast<double>(ctx.parameters.at(1));
			chroma.spill_suppress				= 180.0 - boost::lexical_cast<double>(ctx.parameters.at(3)) * 180.0;
			chroma.spill_suppress_saturation	= 1;

			if (*legacy_mode == chroma::legacy_type::green)
				chroma.target_hue = 120;
			else if (*legacy_mode == chroma::legacy_type::blue)
				chroma.target_hue = 240;
		}
	}
	else
	{
		duration = ctx.parameters.size() > 9 ? boost::lexical_cast<int>(ctx.parameters.at(9)) : 0;
		tween = ctx.parameters.size() > 10 ? ctx.parameters.at(10) : L"linear";

		chroma.enable = ctx.parameters.at(0) == L"1";

		if (chroma.enable)
		{
			chroma.target_hue					= boost::lexical_cast<double>(ctx.parameters.at(1));
			chroma.hue_width					= boost::lexical_cast<double>(ctx.parameters.at(2));
			chroma.min_saturation				= boost::lexical_cast<double>(ctx.parameters.at(3));
			chroma.min_brightness				= boost::lexical_cast<double>(ctx.parameters.at(4));
			chroma.softness						= boost::lexical_cast<double>(ctx.parameters.at(5));
			chroma.spill_suppress				= boost::lexical_cast<double>(ctx.parameters.at(6));
			chroma.spill_suppress_saturation	= boost::lexical_cast<double>(ctx.parameters.at(7));
			chroma.show_mask					= boost::lexical_cast<double>(ctx.parameters.at(8));
		}
	}


	transforms.add(stage::transform_tuple_t(ctx.layer_index(), [=](frame_transform transform) -> frame_transform
	{
		transform.image_transform.chroma = chroma;
		return transform;
	}, duration, tween));
	transforms.apply();

	return L"202 MIXER OK\r\n";
}

std::wstring mixer_blend_command(command_context& ctx)
{
	if (ctx.parameters.empty())
		return reply_value(ctx, [](const frame_transform& t) { return get_blend_mode(t.image_transform.blend_mode); });

	transforms_applier transforms(ctx);
	auto value = get_blend_mode(ctx.parameters.at(0));
	transforms.add(stage::transform_tuple_t(ctx.layer_index(), [=](frame_transform transform) -> frame_transform
	{
		transform.image_transform.blend_mode = value;
		return transform;
	}, 0, tweener(L"linear")));
	transforms.apply();

	return L"202 MIXER OK\r\n";
}

template<typename Getter, typename Setter>
std::wstring single_double_animatable_mixer_command(command_context& ctx, const Getter& getter, const Setter& setter)
{
	if (ctx.parameters.empty())
		return reply_value(ctx, getter);

	transforms_applier transforms(ctx);
	double value = boost::lexical_cast<double>(ctx.parameters.at(0));
	int duration = ctx.parameters.size() > 1 ? boost::lexical_cast<int>(ctx.parameters[1]) : 0;
	std::wstring tween = ctx.parameters.size() > 2 ? ctx.parameters[2] : L"linear";

	transforms.add(stage::transform_tuple_t(ctx.layer_index(), [=](frame_transform transform) -> frame_transform
	{
		setter(transform, value);
		return transform;
	}, duration, tween));
	transforms.apply();

	return L"202 MIXER OK\r\n";
}

std::wstring mixer_opacity_command(command_context& ctx)
{
	return single_double_animatable_mixer_command(
			ctx,
			[](const frame_transform& t) { return t.image_transform.opacity; },
			[](frame_transform& t, double value) { t.image_transform.opacity = value; });
}

std::wstring mixer_brightness_command(command_context& ctx)
{
	return single_double_animatable_mixer_command(
			ctx,
			[](const frame_transform& t) { return t.image_transform.brightness; },
			[](frame_transform& t, double value) { t.image_transform.brightness = value; });
}

std::wstring mixer_saturation_command(command_context& ctx)
{
	return single_double_animatable_mixer_command(
			ctx,
			[](const frame_transform& t) { return t.image_transform.saturation; },
			[](frame_transform& t, double value) { t.image_transform.saturation = value; });
}

std::wstring mixer_contrast_command(command_context& ctx)
{
	return single_double_animatable_mixer_command(
			ctx,
			[](const frame_transform& t) { return t.image_transform.contrast; },
			[](frame_transform& t, double value) { t.image_transform.contrast = value; });
}

std::wstring mixer_levels_command(command_context& ctx)
{
	if (ctx.parameters.empty())
	{
		auto levels = get_current_transform(ctx).image_transform.levels;
		return L"201 MIXER OK\r\n"
			+ boost::lexical_cast<std::wstring>(levels.min_input) + L" "
			+ boost::lexical_cast<std::wstring>(levels.max_input) + L" "
			+ boost::lexical_cast<std::wstring>(levels.gamma) + L" "
			+ boost::lexical_cast<std::wstring>(levels.min_output) + L" "
			+ boost::lexical_cast<std::wstring>(levels.max_output) + L"\r\n";
	}

	transforms_applier transforms(ctx);
	levels value;
	value.min_input = boost::lexical_cast<double>(ctx.parameters.at(0));
	value.max_input = boost::lexical_cast<double>(ctx.parameters.at(1));
	value.gamma = boost::lexical_cast<double>(ctx.parameters.at(2));
	value.min_output = boost::lexical_cast<double>(ctx.parameters.at(3));
	value.max_output = boost::lexical_cast<double>(ctx.parameters.at(4));
	int duration = ctx.parameters.size() > 5 ? boost::lexical_cast<int>(ctx.parameters[5]) : 0;
	std::wstring tween = ctx.parameters.size() > 6 ? ctx.parameters[6] : L"linear";

	transforms.add(stage::transform_tuple_t(ctx.layer_index(), [=](frame_transform transform) -> frame_transform
	{
		transform.image_transform.levels = value;
		return transform;
	}, duration, tween));
	transforms.apply();

	return L"202 MIXER OK\r\n";
}

std::wstring mixer_fill_command(command_context& ctx)
{
	if (ctx.parameters.empty())
	{
		auto transform = get_current_transform(ctx).image_transform;
		auto translation = transform.fill_translation;
		auto scale = transform.fill_scale;
		return L"201 MIXER OK\r\n"
			+ boost::lexical_cast<std::wstring>(translation[0]) + L" "
			+ boost::lexical_cast<std::wstring>(translation[1]) + L" "
			+ boost::lexical_cast<std::wstring>(scale[0]) + L" "
			+ boost::lexical_cast<std::wstring>(scale[1]) + L"\r\n";
	}

	transforms_applier transforms(ctx);
	int duration = ctx.parameters.size() > 4 ? boost::lexical_cast<int>(ctx.parameters[4]) : 0;
	std::wstring tween = ctx.parameters.size() > 5 ? ctx.parameters[5] : L"linear";
	double x = boost::lexical_cast<double>(ctx.parameters.at(0));
	double y = boost::lexical_cast<double>(ctx.parameters.at(1));
	double x_s = boost::lexical_cast<double>(ctx.parameters.at(2));
	double y_s = boost::lexical_cast<double>(ctx.parameters.at(3));

	transforms.add(stage::transform_tuple_t(ctx.layer_index(), [=](frame_transform transform) mutable -> frame_transform
	{
		transform.image_transform.fill_translation[0] = x;
		transform.image_transform.fill_translation[1] = y;
		transform.image_transform.fill_scale[0] = x_s;
		transform.image_transform.fill_scale[1] = y_s;
		return transform;
	}, duration, tween));
	transforms.apply();

	return L"202 MIXER OK\r\n";
}

std::wstring mixer_clip_command(command_context& ctx)
{
	if (ctx.parameters.empty())
	{
		auto transform = get_current_transform(ctx).image_transform;
		auto translation = transform.clip_translation;
		auto scale = transform.clip_scale;

		return L"201 MIXER OK\r\n"
			+ boost::lexical_cast<std::wstring>(translation[0]) + L" "
			+ boost::lexical_cast<std::wstring>(translation[1]) + L" "
			+ boost::lexical_cast<std::wstring>(scale[0]) + L" "
			+ boost::lexical_cast<std::wstring>(scale[1]) + L"\r\n";
	}

	transforms_applier transforms(ctx);
	int duration = ctx.parameters.size() > 4 ? boost::lexical_cast<int>(ctx.parameters[4]) : 0;
	std::wstring tween = ctx.parameters.size() > 5 ? ctx.parameters[5] : L"linear";
	double x = boost::lexical_cast<double>(ctx.parameters.at(0));
	double y = boost::lexical_cast<double>(ctx.parameters.at(1));
	double x_s = boost::lexical_cast<double>(ctx.parameters.at(2));
	double y_s = boost::lexical_cast<double>(ctx.parameters.at(3));

	transforms.add(stage::transform_tuple_t(ctx.layer_index(), [=](frame_transform transform) -> frame_transform
	{
		transform.image_transform.clip_translation[0] = x;
		transform.image_transform.clip_translation[1] = y;
		transform.image_transform.clip_scale[0] = x_s;
		transform.image_transform.clip_scale[1] = y_s;
		return transform;
	}, duration, tween));
	transforms.apply();

	return L"202 MIXER OK\r\n";
}

std::wstring mixer_anchor_command(command_context& ctx)
{
	if (ctx.parameters.empty())
	{
		auto transform = get_current_transform(ctx).image_transform;
		auto anchor = transform.anchor;
		return L"201 MIXER OK\r\n"
			+ boost::lexical_cast<std::wstring>(anchor[0]) + L" "
			+ boost::lexical_cast<std::wstring>(anchor[1]) + L"\r\n";
	}

	transforms_applier transforms(ctx);
	int duration = ctx.parameters.size() > 2 ? boost::lexical_cast<int>(ctx.parameters[2]) : 0;
	std::wstring tween = ctx.parameters.size() > 3 ? ctx.parameters[3] : L"linear";
	double x = boost::lexical_cast<double>(ctx.parameters.at(0));
	double y = boost::lexical_cast<double>(ctx.parameters.at(1));

	transforms.add(stage::transform_tuple_t(ctx.layer_index(), [=](frame_transform transform) mutable -> frame_transform
	{
		transform.image_transform.anchor[0] = x;
		transform.image_transform.anchor[1] = y;
		return transform;
	}, duration, tween));
	transforms.apply();

	return L"202 MIXER OK\r\n";
}

std::wstring mixer_crop_command(command_context& ctx)
{
	if (ctx.parameters.empty())
	{
		auto crop = get_current_transform(ctx).image_transform.crop;
		return L"201 MIXER OK\r\n"
			+ boost::lexical_cast<std::wstring>(crop.ul[0]) + L" "
			+ boost::lexical_cast<std::wstring>(crop.ul[1]) + L" "
			+ boost::lexical_cast<std::wstring>(crop.lr[0]) + L" "
			+ boost::lexical_cast<std::wstring>(crop.lr[1]) + L"\r\n";
	}

	transforms_applier transforms(ctx);
	int duration = ctx.parameters.size() > 4 ? boost::lexical_cast<int>(ctx.parameters[4]) : 0;
	std::wstring tween = ctx.parameters.size() > 5 ? ctx.parameters[5] : L"linear";
	double ul_x = boost::lexical_cast<double>(ctx.parameters.at(0));
	double ul_y = boost::lexical_cast<double>(ctx.parameters.at(1));
	double lr_x = boost::lexical_cast<double>(ctx.parameters.at(2));
	double lr_y = boost::lexical_cast<double>(ctx.parameters.at(3));

	transforms.add(stage::transform_tuple_t(ctx.layer_index(), [=](frame_transform transform) -> frame_transform
	{
		transform.image_transform.crop.ul[0] = ul_x;
		transform.image_transform.crop.ul[1] = ul_y;
		transform.image_transform.crop.lr[0] = lr_x;
		transform.image_transform.crop.lr[1] = lr_y;
		return transform;
	}, duration, tween));
	transforms.apply();

	return L"202 MIXER OK\r\n";
}

std::wstring mixer_rotation_command(command_context& ctx)
{
	static const double PI = 3.141592653589793;

	return single_double_animatable_mixer_command(
			ctx,
			[](const frame_transform& t) { return t.image_transform.angle / PI * 180.0; },
			[](frame_transform& t, double value) { t.image_transform.angle = value * PI / 180.0; });
}

std::wstring mixer_perspective_command(command_context& ctx)
{
	if (ctx.parameters.empty())
	{
		auto perspective = get_current_transform(ctx).image_transform.perspective;
		return
			L"201 MIXER OK\r\n"
			+ boost::lexical_cast<std::wstring>(perspective.ul[0]) + L" "
			+ boost::lexical_cast<std::wstring>(perspective.ul[1]) + L" "
			+ boost::lexical_cast<std::wstring>(perspective.ur[0]) + L" "
			+ boost::lexical_cast<std::wstring>(perspective.ur[1]) + L" "
			+ boost::lexical_cast<std::wstring>(perspective.lr[0]) + L" "
			+ boost::lexical_cast<std::wstring>(perspective.lr[1]) + L" "
			+ boost::lexical_cast<std::wstring>(perspective.ll[0]) + L" "
			+ boost::lexical_cast<std::wstring>(perspective.ll[1]) + L"\r\n";
	}

	transforms_applier transforms(ctx);
	int duration = ctx.parameters.size() > 8 ? boost::lexical_cast<int>(ctx.parameters[8]) : 0;
	std::wstring tween = ctx.parameters.size() > 9 ? ctx.parameters[9] : L"linear";
	double ul_x = boost::lexical_cast<double>(ctx.parameters.at(0));
	double ul_y = boost::lexical_cast<double>(ctx.parameters.at(1));
	double ur_x = boost::lexical_cast<double>(ctx.parameters.at(2));
	double ur_y = boost::lexical_cast<double>(ctx.parameters.at(3));
	double lr_x = boost::lexical_cast<double>(ctx.parameters.at(4));
	double lr_y = boost::lexical_cast<double>(ctx.parameters.at(5));
	double ll_x = boost::lexical_cast<double>(ctx.parameters.at(6));
	double ll_y = boost::lexical_cast<double>(ctx.parameters.at(7));

	transforms.add(stage::transform_tuple_t(ctx.layer_index(), [=](frame_transform transform) -> frame_transform
	{
		transform.image_transform.perspective.ul[0] = ul_x;
		transform.image_transform.perspective.ul[1] = ul_y;
		transform.image_transform.perspective.ur[0] = ur_x;
		transform.image_transform.perspective.ur[1] = ur_y;
		transform.image_transform.perspective.lr[0] = lr_x;
		transform.image_transform.perspective.lr[1] = lr_y;
		transform.image_transform.perspective.ll[0] = ll_x;
		transform.image_transform.perspective.ll[1] = ll_y;
		return transform;
	}, duration, tween));
	transforms.apply();

	return L"202 MIXER OK\r\n";
}

std::wstring mixer_mipmap_command(command_context& ctx)
{
	if (ctx.parameters.empty())
		return reply_value(ctx, [](const frame_transform& t) { return t.image_transform.use_mipmap ? 1 : 0; });

	transforms_applier transforms(ctx);
	bool value = boost::lexical_cast<int>(ctx.parameters.at(0));
	transforms.add(stage::transform_tuple_t(ctx.layer_index(), [=](frame_transform transform) -> frame_transform
	{
		transform.image_transform.use_mipmap = value;
		return transform;
	}, 0, tweener(L"linear")));
	transforms.apply();

	return L"202 MIXER OK\r\n";
}

std::wstring mixer_volume_command(command_context& ctx)
{
	return single_double_animatable_mixer_command(
		ctx,
		[](const frame_transform& t) { return t.audio_transform.volume; },
		[](frame_transform& t, double value) { t.audio_transform.volume = value; });
}

std::wstring mixer_mastervolume_command(command_context& ctx)
{
	if (ctx.parameters.empty())
	{
		auto volume = ctx.channel.channel->mixer().get_master_volume();
		return L"201 MIXER OK\r\n" + boost::lexical_cast<std::wstring>(volume)+L"\r\n";
	}

	float master_volume = boost::lexical_cast<float>(ctx.parameters.at(0));
	ctx.channel.channel->mixer().set_master_volume(master_volume);

	return L"202 MIXER OK\r\n";
}

std::wstring mixer_straight_alpha_command(command_context& ctx)
{
	if (ctx.parameters.empty())
	{
		bool state = ctx.channel.channel->mixer().get_straight_alpha_output();
		return L"201 MIXER OK\r\n" + boost::lexical_cast<std::wstring>(state) + L"\r\n";
	}

	bool state = boost::lexical_cast<bool>(ctx.parameters.at(0));
	ctx.channel.channel->mixer().set_straight_alpha_output(state);

	return L"202 MIXER OK\r\n";
}

std::wstring mixer_grid_command(command_context& ctx)
{
	transforms_applier transforms(ctx);
	int duration = ctx.parameters.size() > 1 ? boost::lexical_cast<int>(ctx.parameters[1]) : 0;
	std::wstring tween = ctx.parameters.size() > 2 ? ctx.parameters[2] : L"linear";
	int n = boost::lexical_cast<int>(ctx.parameters.at(0));
	double delta = 1.0 / static_cast<double>(n);
	for (int x = 0; x < n; ++x)
	{
		for (int y = 0; y < n; ++y)
		{
			int index = x + y*n + 1;
			transforms.add(stage::transform_tuple_t(index, [=](frame_transform transform) -> frame_transform
			{
				transform.image_transform.fill_translation[0] = x*delta;
				transform.image_transform.fill_translation[1] = y*delta;
				transform.image_transform.fill_scale[0] = delta;
				transform.image_transform.fill_scale[1] = delta;
				transform.image_transform.clip_translation[0] = x*delta;
				transform.image_transform.clip_translation[1] = y*delta;
				transform.image_transform.clip_scale[0] = delta;
				transform.image_transform.clip_scale[1] = delta;
				return transform;
			}, duration, tween));
		}
	}
	transforms.apply();

	return L"202 MIXER OK\r\n";
}

std::wstring mixer_commit_command(command_context& ctx)
{
	transforms_applier transforms(ctx);
	transforms.commit_deferred();

	return L"202 MIXER OK\r\n";
}

std::wstring mixer_clear_command(command_context& ctx)
{
	int layer = ctx.layer_id;

	if (layer == -1)
		ctx.channel.channel->stage().clear_transforms();
	else
		ctx.channel.channel->stage().clear_transforms(layer);

	return L"202 MIXER OK\r\n";
}

std::wstring channel_grid_command(command_context& ctx)
{
	int index = 1;
	auto self = ctx.channels.back();

	core::diagnostics::scoped_call_context save;
	core::diagnostics::call_context::for_thread().video_channel = ctx.channels.size();

	std::vector<std::wstring> params;
	params.push_back(L"SCREEN");
	params.push_back(L"0");
	params.push_back(L"NAME");
	params.push_back(L"Channel Grid Window");
	auto screen = ctx.consumer_registry->create_consumer(params, &self.channel->stage(), get_channels(ctx));

	self.channel->output().add(screen);

	for (auto& channel : ctx.channels)
	{
		if (channel.channel != self.channel)
		{
			core::diagnostics::call_context::for_thread().layer = index;
			auto producer = ctx.producer_registry->create_producer(get_producer_dependencies(self.channel, ctx), L"route://" + boost::lexical_cast<std::wstring>(channel.channel->index()) + L" NO_AUTO_DEINTERLACE");
			self.channel->stage().load(index, producer, false);
			self.channel->stage().play(index);
			index++;
		}
	}

	auto num_channels = ctx.channels.size() - 1;
	int square_side_length = std::ceil(std::sqrt(num_channels));

	ctx.channel_index = self.channel->index();
	ctx.channel = self;
	ctx.parameters.clear();
	ctx.parameters.push_back(boost::lexical_cast<std::wstring>(square_side_length));
	mixer_grid_command(ctx);

	return L"202 CHANNEL_GRID OK\r\n";
}

// Thumbnail Commands

std::wstring thumbnail_list_command(command_context& ctx)
{
	std::wstring sub_directory;

	if (!ctx.parameters.empty())
		sub_directory = ctx.parameters.at(0);

	std::wstringstream replyString;
	replyString << L"200 THUMBNAIL LIST OK\r\n";

	for (boost::filesystem::recursive_directory_iterator itr(get_sub_directory(env::thumbnail_folder(), sub_directory)), end; itr != end; ++itr)
	{
		if (boost::filesystem::is_regular_file(itr->path()))
		{
			if (!boost::iequals(itr->path().extension().wstring(), L".png"))
				continue;

			auto relativePath = get_relative_without_extension(itr->path(), env::thumbnail_folder());
			auto str = relativePath.generic_wstring();

			if (str[0] == '\\' || str[0] == '/')
				str = std::wstring(str.begin() + 1, str.end());

			auto mtime = boost::filesystem::last_write_time(itr->path());
			auto mtime_readable = boost::posix_time::to_iso_wstring(boost::posix_time::from_time_t(mtime));
			auto file_size = boost::filesystem::file_size(itr->path());

			replyString << L"\"" << str << L"\" " << mtime_readable << L" " << file_size << L"\r\n";
		}
	}

	replyString << L"\r\n";

	return boost::to_upper_copy(replyString.str());
}
std::wstring thumbnail_retrieve_command(command_context& ctx)
{
	std::wstring filename = env::thumbnail_folder();
	filename.append(ctx.parameters.at(0));
	filename.append(L".png");

	std::wstring file_contents;

	auto found_file = find_case_insensitive(filename);

	if (found_file)
		file_contents = read_file_base64(boost::filesystem::path(*found_file));

	if (file_contents.empty())
		CASPAR_THROW_EXCEPTION(file_not_found() << msg_info(filename + L" not found"));

	std::wstringstream reply;

	reply << L"201 THUMBNAIL RETRIEVE OK\r\n";
	reply << file_contents;
	reply << L"\r\n";
	return reply.str();
}

std::wstring thumbnail_generate_command(command_context& ctx)
{
	CASPAR_THROW_EXCEPTION(not_supported() << msg_info(L"Thumbnail generation turned off"));
}

std::wstring thumbnail_generateall_command(command_context& ctx)
{
	CASPAR_THROW_EXCEPTION(not_supported() << msg_info(L"Thumbnail generation turned off"));
}

// Query Commands

std::wstring cinf_command(command_context& ctx)
{
    CASPAR_THROW_EXCEPTION(not_supported() << msg_info(L"cinf turned off"));
}

std::wstring cls_command(command_context& ctx)
{
    CASPAR_THROW_EXCEPTION(not_supported() << msg_info(L"cls turned off"));
}

std::wstring fls_command(command_context& ctx)
{
	std::wstringstream replyString;
	replyString << L"200 FLS OK\r\n";

	replyString << L"\r\n";

	return replyString.str();
}


std::wstring tls_command(command_context& ctx)
{
    CASPAR_THROW_EXCEPTION(not_supported() << msg_info(L"tls turned off"));
}

std::wstring version_command(command_context& ctx)
{
	return L"201 VERSION OK\r\n" + env::version() + L"\r\n";
}

std::wstring diag_command(command_context& ctx)
{
	core::diagnostics::osd::show_graphs(true);

	return L"202 DIAG OK\r\n";
}

std::wstring bye_command(command_context& ctx)
{
	ctx.client->disconnect();
	return L"";
}

std::wstring kill_command(command_context& ctx)
{
	ctx.shutdown_server_now.set_value(false);	//false for not attempting to restart
	return L"202 KILL OK\r\n";
}

std::wstring restart_command(command_context& ctx)
{
	ctx.shutdown_server_now.set_value(true);	//true for attempting to restart
	return L"202 RESTART OK\r\n";
}

std::wstring lock_command(command_context& ctx)
{
	int channel_index = boost::lexical_cast<int>(ctx.parameters.at(0)) - 1;
	auto lock = ctx.channels.at(channel_index).lock;
	auto command = boost::to_upper_copy(ctx.parameters.at(1));

	if (command == L"ACQUIRE")
	{
		std::wstring lock_phrase = ctx.parameters.at(2);

		//TODO: read options

		//just lock one channel
		if (!lock->try_lock(lock_phrase, ctx.client))
			return L"503 LOCK ACQUIRE FAILED\r\n";

		return L"202 LOCK ACQUIRE OK\r\n";
	}
	else if (command == L"RELEASE")
	{
		lock->release_lock(ctx.client);
		return L"202 LOCK RELEASE OK\r\n";
	}
	else if (command == L"CLEAR")
	{
		std::wstring override_phrase = env::properties().get(L"configuration.lock-clear-phrase", L"");
		std::wstring client_override_phrase;

		if (!override_phrase.empty())
			client_override_phrase = ctx.parameters.at(2);

		//just clear one channel
		if (client_override_phrase != override_phrase)
			return L"503 LOCK CLEAR FAILED\r\n";

		lock->clear_locks();

		return L"202 LOCK CLEAR OK\r\n";
	}

	CASPAR_THROW_EXCEPTION(file_not_found() << msg_info(L"Unknown LOCK command " + command));
}

void register_commands(amcp_command_repository& repo)
{
    repo.register_channel_command(L"Basic Commands", L"LOADBG", loadbg_command, 1);
    repo.register_channel_command(L"Basic Commands", L"LOAD", load_command, 1);
    repo.register_channel_command(L"Basic Commands", L"PLAY", play_command, 0);
    repo.register_channel_command(L"Basic Commands", L"PAUSE", pause_command, 0);
    repo.register_channel_command(L"Basic Commands", L"RESUME", resume_command, 0);
    repo.register_channel_command(L"Basic Commands", L"STOP", stop_command, 0);
    repo.register_channel_command(L"Basic Commands", L"CLEAR", clear_command, 0);
    repo.register_channel_command(L"Basic Commands", L"CALL", call_command, 1);
    repo.register_channel_command(L"Basic Commands", L"SWAP", swap_command, 1);
    repo.register_channel_command(L"Basic Commands", L"ADD", add_command, 1);
    repo.register_channel_command(L"Basic Commands", L"REMOVE", remove_command, 0);
    repo.register_channel_command(L"Basic Commands", L"PRINT", print_command, 0);
    repo.register_command(L"Basic Commands", L"LOG LEVEL", log_level_command, 1);
    repo.register_command(L"Basic Commands", L"LOG CATEGORY", log_category_command, 2);
    repo.register_channel_command(L"Basic Commands", L"SET", set_command, 2);
    repo.register_command(L"Basic Commands", L"LOCK", lock_command, 2);

    repo.register_command(L"Data Commands", L"DATA STORE", data_store_command, 2);
    repo.register_command(L"Data Commands", L"DATA RETRIEVE", data_retrieve_command, 1);
    repo.register_command(L"Data Commands", L"DATA LIST", data_list_command, 0);
    repo.register_command(L"Data Commands", L"DATA REMOVE", data_remove_command, 1);

    repo.register_channel_command(L"Template Commands", L"CG ADD", cg_add_command, 3);
    repo.register_channel_command(L"Template Commands", L"CG PLAY", cg_play_command, 1);
    repo.register_channel_command(L"Template Commands", L"CG STOP", cg_stop_command, 1);
    repo.register_channel_command(L"Template Commands", L"CG NEXT", cg_next_command, 1);
    repo.register_channel_command(L"Template Commands", L"CG REMOVE", cg_remove_command, 1);
    repo.register_channel_command(L"Template Commands", L"CG CLEAR", cg_clear_command, 0);
    repo.register_channel_command(L"Template Commands", L"CG UPDATE", cg_update_command, 2);
    repo.register_channel_command(L"Template Commands", L"CG INVOKE", cg_invoke_command, 2);
    repo.register_channel_command(L"Template Commands", L"CG INFO", cg_info_command, 0);

    repo.register_channel_command(L"Mixer Commands", L"MIXER KEYER", mixer_keyer_command, 0);
    repo.register_channel_command(L"Mixer Commands", L"MIXER CHROMA", mixer_chroma_command, 0);
    repo.register_channel_command(L"Mixer Commands", L"MIXER BLEND", mixer_blend_command, 0);
    repo.register_channel_command(L"Mixer Commands", L"MIXER OPACITY", mixer_opacity_command, 0);
    repo.register_channel_command(L"Mixer Commands", L"MIXER BRIGHTNESS", mixer_brightness_command, 0);
    repo.register_channel_command(L"Mixer Commands", L"MIXER SATURATION", mixer_saturation_command, 0);
    repo.register_channel_command(L"Mixer Commands", L"MIXER CONTRAST", mixer_contrast_command, 0);
    repo.register_channel_command(L"Mixer Commands", L"MIXER LEVELS", mixer_levels_command, 0);
    repo.register_channel_command(L"Mixer Commands", L"MIXER FILL", mixer_fill_command, 0);
    repo.register_channel_command(L"Mixer Commands", L"MIXER CLIP", mixer_clip_command, 0);
    repo.register_channel_command(L"Mixer Commands", L"MIXER ANCHOR", mixer_anchor_command, 0);
    repo.register_channel_command(L"Mixer Commands", L"MIXER CROP", mixer_crop_command, 0);
    repo.register_channel_command(L"Mixer Commands", L"MIXER ROTATION", mixer_rotation_command, 0);
    repo.register_channel_command(L"Mixer Commands", L"MIXER PERSPECTIVE", mixer_perspective_command, 0);
    repo.register_channel_command(L"Mixer Commands", L"MIXER MIPMAP", mixer_mipmap_command, 0);
    repo.register_channel_command(L"Mixer Commands", L"MIXER VOLUME", mixer_volume_command, 0);
    repo.register_channel_command(L"Mixer Commands", L"MIXER MASTERVOLUME", mixer_mastervolume_command, 0);
    repo.register_channel_command(L"Mixer Commands", L"MIXER STRAIGHT_ALPHA_OUTPUT", mixer_straight_alpha_command, 0);
    repo.register_channel_command(L"Mixer Commands", L"MIXER GRID", mixer_grid_command, 1);
    repo.register_channel_command(L"Mixer Commands", L"MIXER COMMIT", mixer_commit_command, 0);
    repo.register_channel_command(L"Mixer Commands", L"MIXER CLEAR", mixer_clear_command, 0);
    repo.register_command(L"Mixer Commands", L"CHANNEL_GRID", channel_grid_command, 0);

    repo.register_command(L"Thumbnail Commands", L"THUMBNAIL LIST", thumbnail_list_command, 0);
    repo.register_command(L"Thumbnail Commands", L"THUMBNAIL RETRIEVE", thumbnail_retrieve_command, 1);
    repo.register_command(L"Thumbnail Commands", L"THUMBNAIL GENERATE", thumbnail_generate_command, 1);
    repo.register_command(L"Thumbnail Commands", L"THUMBNAIL GENERATE_ALL", thumbnail_generateall_command, 0);

    repo.register_command(L"Query Commands", L"CINF", cinf_command, 1);
    repo.register_command(L"Query Commands", L"CLS", cls_command, 0);
    repo.register_command(L"Query Commands", L"FLS", fls_command, 0);
    repo.register_command(L"Query Commands", L"TLS", tls_command, 0);
    repo.register_command(L"Query Commands", L"VERSION", version_command, 0);
    repo.register_command(L"Query Commands", L"DIAG", diag_command, 0);
    repo.register_command(L"Query Commands", L"BYE", bye_command, 0);
    repo.register_command(L"Query Commands", L"KILL", kill_command, 0);
    repo.register_command(L"Query Commands", L"RESTART", restart_command, 0);
}

}}}
