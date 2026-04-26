// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "core/playback/OutputMenuModel.h"

namespace app::core::playback
{

  void OutputMenuModel::update(std::vector<BackendSnapshot> const& backends,
                               BackendKind currentBackend,
                               std::string_view currentDeviceId)
  {
    auto const deviceIdStr = std::string(currentDeviceId);

    bool const changed =
      (_backends != backends) || (_currentBackend != currentBackend) || (_currentDeviceId != deviceIdStr);

    if (changed)
    {
      _backends = backends;
      _currentBackend = currentBackend;
      _currentDeviceId = deviceIdStr;
      _signalChanged.emit();
    }
  }

} // namespace app::core::playback
