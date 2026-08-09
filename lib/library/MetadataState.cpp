// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "MetadataState.h"

#include <shared_mutex>

namespace ao::library::detail
{
  MetadataSnapshot MetadataState::snapshot() const
  {
    auto const lock = std::shared_lock{_mutex};
    return {.header = _header, .revision = _revision};
  }
} // namespace ao::library::detail
