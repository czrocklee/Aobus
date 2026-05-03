// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "OutputMenuModel.h"

namespace ao::gtk
{
  void OutputMenuModel::update(std::vector<ao::audio::IBackendProvider::Status> const& backends,
                               ao::audio::BackendId const& currentBackend,
                               ao::audio::ProfileId const& currentProfile,
                               std::string_view currentDeviceId)
  {
    auto const deviceIdStr = std::string{currentDeviceId};

    bool const changed = (_backends != backends) || (_currentBackend != currentBackend) ||
                         (_currentProfile != currentProfile) || (_currentDeviceId != deviceIdStr);

    if (changed)
    {
      _backends = backends;
      _currentBackend = currentBackend;
      _currentProfile = currentProfile;
      _currentDeviceId = deviceIdStr;
      _signalChanged.emit();
    }
  }
} // namespace ao::gtk