/*
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
 */
#include "omt.h"

#include "consumer/omt_consumer.h"
#include "producer/omt_producer.h"
#include "util/omt_util.h"

#include <core/consumer/frame_consumer.h>
#include <protocol/amcp/amcp_command_repository_wrapper.h>

#include <common/env.h>
#include <common/log.h>

#include <boost/property_tree/ptree.hpp>

namespace caspar { namespace omt {

void init(const core::module_dependencies& dependencies)
{
    try {
        dependencies.consumer_registry->register_consumer_factory(L"OMT Consumer", create_omt_consumer);
        dependencies.consumer_registry->register_preconfigured_consumer_factory(L"omt",
                                                                                create_preconfigured_omt_consumer);

        dependencies.producer_registry->register_producer_factory(L"OMT Producer", create_omt_producer);

        dependencies.command_repository->register_command(L"Query Commands", L"OMT LIST", omt::list_command, 0);

        const bool auto_load = caspar::env::properties().get(L"configuration.omt.auto-load", false);
        if (auto_load == true) {
            omt::load_library();
        }
    } catch (...) {
        CASPAR_LOG_CURRENT_EXCEPTION();
    }
}

}} // namespace caspar::omt
