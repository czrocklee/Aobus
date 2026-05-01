// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/audio/PlaybackTypes.h>

#include <sigc++/sigc++.h>

#include <string>
#include <string_view>
#include <vector>

namespace app::ui
{
  class OutputMenuModel final : public sigc::trackable
  {
  public:
    OutputMenuModel() = default;
    ~OutputMenuModel() = default;

    void update(std::vector<rs::audio::BackendSnapshot> const& backends,
                rs::audio::BackendKind currentBackend,
                std::string_view currentDeviceId);

    std::vector<rs::audio::BackendSnapshot> const& getBackends() const noexcept { return _backends; }
    rs::audio::BackendKind getCurrentBackend() const noexcept { return _currentBackend; }
    std::string const& getCurrentDeviceId() const noexcept { return _currentDeviceId; }

    using ChangedSignal = sigc::signal<void()>;
    ChangedSignal& signalChanged() { return _signalChanged; }

  private:
    std::vector<rs::audio::BackendSnapshot> _backends;
    rs::audio::BackendKind _currentBackend = rs::audio::BackendKind::None;
    std::string _currentDeviceId;

    ChangedSignal _signalChanged;
  };
} // namespace app::ui