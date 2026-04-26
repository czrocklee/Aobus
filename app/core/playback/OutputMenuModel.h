// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "core/playback/PlaybackTypes.h"

#include <sigc++/sigc++.h>

#include <string>
#include <string_view>
#include <vector>

namespace app::core::playback
{

  class OutputMenuModel final : public sigc::trackable
  {
  public:
    OutputMenuModel() = default;
    ~OutputMenuModel() = default;

    void update(std::vector<BackendSnapshot> const& backends,
                BackendKind currentBackend,
                std::string_view currentDeviceId);

    std::vector<BackendSnapshot> const& getBackends() const noexcept { return _backends; }
    BackendKind getCurrentBackend() const noexcept { return _currentBackend; }
    std::string const& getCurrentDeviceId() const noexcept { return _currentDeviceId; }

    using ChangedSignal = sigc::signal<void()>;
    ChangedSignal& signalChanged() { return _signalChanged; }

  private:
    std::vector<BackendSnapshot> _backends;
    BackendKind _currentBackend = BackendKind::None;
    std::string _currentDeviceId;

    ChangedSignal _signalChanged;
  };

} // namespace app::core::playback
