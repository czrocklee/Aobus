// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/utility/StrongType.h>

#include <string>

namespace ao::audio
{
  using BackendId = utility::StrongType<std::string, struct BackendTag>;
  using ProfileId = utility::StrongType<std::string, struct ProfileTag>;

  // These owning vocabulary values live for the process lifetime.
  inline BackendId const kBackendNone{""};
  inline BackendId const kBackendPipeWire{"pipewire"};
  inline BackendId const kBackendAlsa{"alsa"};
  inline BackendId const kBackendWasapi{"wasapi"};

  inline ProfileId const kProfileShared{"shared"};
  inline ProfileId const kProfileExclusive{"exclusive"};
} // namespace ao::audio
