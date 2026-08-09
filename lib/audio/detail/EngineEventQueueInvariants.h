// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <cstddef>

namespace ao::audio::detail
{
  struct EngineEventQueueDestructionState final
  {
    bool workerJoinable = false;
    bool running = false;
    bool playbackEventsEmpty = true;
    std::size_t rtSignalCount = 0;
  };

  void verifyEngineEventQueueDestruction(EngineEventQueueDestructionState state) noexcept;
} // namespace ao::audio::detail
