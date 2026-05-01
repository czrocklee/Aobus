// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "platform/linux/ui/OutputMenuModel.h"

namespace app::ui
{
  void OutputMenuModel::update(std::vector<rs::audio::BackendSnapshot> const& backends,
                               rs::audio::BackendKind currentBackend,
                               std::string_view currentDeviceId)
  {
    auto const deviceIdStr = std::string{currentDeviceId};

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
} // namespace app::ui