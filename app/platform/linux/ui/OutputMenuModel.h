// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <ao/audio/Types.h>

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

    void update(std::vector<ao::audio::BackendSnapshot> const& backends,
                ao::audio::BackendKind currentBackend,
                std::string_view currentDeviceId);

    std::vector<ao::audio::BackendSnapshot> const& getBackends() const noexcept { return _backends; }
    ao::audio::BackendKind getCurrentBackend() const noexcept { return _currentBackend; }
    std::string const& getCurrentDeviceId() const noexcept { return _currentDeviceId; }

    using ChangedSignal = sigc::signal<void()>;
    ChangedSignal& signalChanged() { return _signalChanged; }

  private:
    std::vector<ao::audio::BackendSnapshot> _backends;
    ao::audio::BackendKind _currentBackend = ao::audio::BackendKind::None;
    std::string _currentDeviceId;

    ChangedSignal _signalChanged;
  };
} // namespace app::ui