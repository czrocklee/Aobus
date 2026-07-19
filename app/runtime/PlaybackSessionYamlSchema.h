// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <ryml.hpp>

namespace ao::rt
{
  struct PlaybackSessionState;

  struct PlaybackSessionYamlSchema final
  {
    Result<> serialize(ryml::NodeRef node, PlaybackSessionState const& state) const;
    Result<PlaybackSessionState> deserialize(ryml::ConstNodeRef node, PlaybackSessionState const& seed) const;
  };
} // namespace ao::rt
