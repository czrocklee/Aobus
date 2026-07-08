// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/audio/BackendIds.h>

#include <string>

namespace ao::audio
{
  struct RouteAnchor final
  {
    BackendId backend;
    std::string id;

    bool operator==(RouteAnchor const&) const = default;
  };
} // namespace ao::audio
