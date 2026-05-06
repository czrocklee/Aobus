// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "StateTypes.h"
#include <optional>
#include <string>
#include <vector>

namespace ao::app
{
  struct SessionSnapshot final
  {
    std::string lastLibraryPath;
    std::string lastBackend;
    std::string lastProfile;
    std::string lastOutputDeviceId;

    std::vector<TrackListViewConfig> openViews;
    std::optional<std::size_t> activeViewIndex;
  };

  class ISessionPersistence
  {
  public:
    virtual ~ISessionPersistence() = default;

    virtual std::optional<SessionSnapshot> loadSnapshot() = 0;
    virtual void saveSnapshot(SessionSnapshot const& snapshot) = 0;
  };
}