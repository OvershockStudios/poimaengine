// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace poima {
class WorldSession;
// Pump one already prepared owner through the existing bounded NDJSON framing.
// EOF or session.close ends this dedicated lifetime; no endpoint/player pump.
int run_world_stdio(WorldSession& session);
}
