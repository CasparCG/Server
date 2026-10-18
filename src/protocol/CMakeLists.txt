cmake_minimum_required (VERSION 3.28)
project (protocol)

set(SOURCES
		amcp/AMCPCommand.cpp
		amcp/AMCPCommandQueue.cpp
		amcp/AMCPCommandsImpl.cpp
		amcp/AMCPProtocolStrategy.cpp
		amcp/amcp_command_repository.cpp
		amcp/amcp_args.cpp
		amcp/amcp_command_repository_wrapper.cpp

		osc/oscpack/OscOutboundPacketStream.cpp
		osc/oscpack/OscPrintReceivedElements.cpp
		osc/oscpack/OscReceivedElements.cpp
		osc/oscpack/OscTypes.cpp

		osc/client.cpp

		util/AsyncEventServer.cpp
		util/lock_container.cpp
		util/strategy_adapters.cpp
		util/http_request.cpp
		util/tokenize.cpp
)

set(HEADERS
		amcp/AMCPCommand.h
		amcp/AMCPCommandQueue.h
		amcp/AMCPCommandsImpl.h
		amcp/AMCPProtocolStrategy.h
		amcp/amcp_command_repository.h
		amcp/amcp_command_repository_wrapper.h
		amcp/amcp_shared.h
		amcp/amcp_args.h
		amcp/amcp_command_context.h

		osc/oscpack/MessageMappingOscPacketListener.h
		osc/oscpack/OscException.h
		osc/oscpack/OscHostEndianness.h
		osc/oscpack/OscOutboundPacketStream.h
		osc/oscpack/OscPacketListener.h
		osc/oscpack/OscPrintReceivedElements.h
		osc/oscpack/OscReceivedElements.h
		osc/oscpack/OscTypes.h

		osc/client.h

		util/AsyncEventServer.h
		util/ClientInfo.h
		util/lock_container.h
		util/ProtocolStrategy.h
		util/protocol_strategy.h
		util/strategy_adapters.h
		util/http_request.h
		util/tokenize.h

		StdAfx.h
)

casparcg_add_library(protocol SOURCES ${SOURCES} ${HEADERS})
target_include_directories(protocol  PRIVATE
    ..
    )
target_precompile_headers(protocol PRIVATE "StdAfx.h")

source_group(sources\\amcp amcp/*)
source_group(sources\\cii cii/*)
source_group(sources\\clk clk/*)
source_group(sources\\log log/*)
source_group(sources\\osc\\oscpack osc/oscpack/*)
source_group(sources\\osc osc/*)
source_group(sources\\util util/*)
source_group(sources ./*)

target_link_libraries(protocol PRIVATE common core)

if(BUILD_TESTING)
    add_executable(osc_client_test osc/client_test.cpp)
    set_target_properties(osc_client_test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    target_include_directories(osc_client_test PRIVATE ..)
    target_include_directories(osc_client_test SYSTEM PRIVATE ${BOOST_INCLUDE_PATH})
    target_link_libraries(osc_client_test PRIVATE protocol)
    casparcg_add_build_dependencies(osc_client_test)
    if(MSVC)
        target_link_libraries(osc_client_test PRIVATE Ws2_32.lib)
    else()
        target_link_libraries(osc_client_test PRIVATE ${Boost_LIBRARIES} pthread)
    endif()
    add_test(NAME osc_client COMMAND osc_client_test)
    set_tests_properties(osc_client PROPERTIES TIMEOUT 30)
endif()
