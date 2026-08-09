// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "EngineEventQueueInvariants.h"

#include <ao/Contract.h>

namespace ao::audio::detail
{
  void verifyEngineEventQueueDestruction(EngineEventQueueDestructionState const state) noexcept
  {
    AO_INVARIANT(!state.workerJoinable, "EngineEventQueue owner must stop the queue before destruction");
    AO_INVARIANT(!state.running, "EngineEventQueue worker must exit before queue destruction");
    AO_INVARIANT(state.playbackEventsEmpty, "EngineEventQueue owner must clear playback events before destruction");
    AO_INVARIANT(state.rtSignalCount == 0, "EngineEventQueue owner must drain RT signals before destruction");
  }
} // namespace ao::audio::detail
