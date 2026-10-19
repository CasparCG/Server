/*
 * Copyright (c) 2026 Sveriges Television AB <info@casparcg.com>
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
 */

#pragma once

#include <core/monitor/monitor.h>

namespace caspar::core::diagnostics::osc_graph {

// Registers a graph_sink factory that mirrors every caspar::diagnostics::graph's text/values/colors/tags,
// so it can be polled (see snapshot_state()) and broadcast over OSC to clients that opted in via
// `OSC SUBSCRIBE <port> DIAG` (protocol::osc::client). This is independent of, and much cheaper than, the
// SFML osd window - it does no rendering, and costs nothing beyond what set_value() et al already cost
// regardless of whether anyone is subscribed.
void register_sink();

// A snapshot of every currently-live graph's text/values/colors/tags, addressed as:
//   /diag/<id>/channel        - the video channel the graph belongs to, or -1
//   /diag/<id>/layer          - the layer the graph belongs to, or -1
//   /diag/<id>/text           - the graph's label, matching the SFML window's corner text
//   /diag/<id>/value/<name>   - the current value (0..1) of a named series
//   /diag/<id>/color/<name>   - the configured color of a named series, same encoding as get_sfml_color
//   /diag/<id>/tag/<name>     - severity ("warning"/"info"/"silent") of a tag that fired since the last call
// Reports each graph's latest values (which remain until changed or explicitly reset) and clears tags
// after reporting them once. Drops bookkeeping for any graph whose owning caspar::diagnostics::graph has
// since been destroyed.
core::monitor::state snapshot_state();

} // namespace caspar::core::diagnostics::osc_graph
