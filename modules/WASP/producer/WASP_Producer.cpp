
#include "..\stdafx.h"
#include "WASP_Producer.h"


#include "WASP_Memory.h"


CWASP_Memory * g_pWaspMemory;
// TODO: reference additional headers your program requires here
// Header Containing Information about Image in Shared Memory


using namespace boost::assign;

namespace caspar { namespace WASP {

struct WASP_producer : public core::frame_producer
{
	const std::wstring description_;
	std::wstring layerindex;
	core::monitor::subject		monitor_subject_;
	const safe_ptr<core::frame_factory> frame_factory_;	safe_ptr<core::basic_frame> frame_;
	

	 WASP_producer(const safe_ptr<core::frame_factory>& frame_factory, const core::parameters& params) 
		: frame_factory_(frame_factory)
	{
		layerindex = params.get_original_string();

		CASPAR_LOG(info) <<"#######  WASP WASP_producer"<<layerindex;
		if(!g_pWaspMemory)
		{
			g_pWaspMemory = new CWASP_Memory();
			g_pWaspMemory->GetSharedMemoryHandles();

		}

		//GetSharedMemoryHandles();
		
	}
	 virtual safe_ptr<core::basic_frame> receive(int) override
	{
		//CASPAR_LOG(info) <<"#######  WASP receive";

		try
		{
			if(g_pWaspMemory)
				return g_pWaspMemory->receive(frame_factory_);
		}
		catch(...)
		{
			CASPAR_LOG(info) <<"#######Exception in   WASP receive";

		}
		
	}
		
	virtual safe_ptr<core::basic_frame> last_frame() const override
	{
		return frame_;
	}

	virtual safe_ptr<core::basic_frame> create_thumbnail_frame() override
	{
		return frame_;
	}
		
	virtual std::wstring print() const override
	{
		return L"WASP_producer[" + description_ + L"]";
	}

	virtual boost::property_tree::wptree info() const override
	{
		boost::property_tree::wptree info;
		info.add(L"type", L"WASP-producer");
		info.add(L"location", description_);
		return info;
	}

	core::monitor::subject& monitor_output()
	{
		return monitor_subject_;
	}

	virtual boost::unique_future<std::wstring>  call(const std::wstring& param) override 
	{
		std::wstring result = L"";
		//CASPAR_LOG(info) <<L"#######  WASP  call "<< param;
		std::wstring msg = L"";
		try
		{
			if(g_pWaspMemory)
			{
				g_pWaspMemory->SendCommandToPipe(param);
			}
		}
		catch(...)
		{
			CASPAR_LOG(info) <<L"@@@@  Exception WASP  call "<< msg;	
		}
		return wrap_as_future(std::wstring(result));
	}
};

safe_ptr<core::frame_producer> create_raw_producer(
	const safe_ptr<core::frame_factory>& frame_factory,
	const core::parameters& params)
{
	return make_safe<WASP_producer>(frame_factory,params);
}

safe_ptr<core::frame_producer> create_producer(
		const safe_ptr<core::frame_factory>& frame_factory,
		const core::parameters& params)
{
	CASPAR_LOG(info) <<L"#######  WASP  create_producer" <<params.get_original_string();

	auto raw_producer = create_raw_producer(frame_factory, params);

	if (raw_producer == core::frame_producer::empty())
		return raw_producer;

	return create_producer_print_proxy(raw_producer);		

	
}

safe_ptr<core::frame_producer> create_thumbnail_producer(
		const safe_ptr<core::frame_factory>& frame_factory,
		const core::parameters& params)
{
	CASPAR_LOG(info) <<"#######  WASP  create_thumbnail_producer";
	return create_raw_producer(frame_factory, params);
}






}}