// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/audio/IBackendProvider.h>
#include <ao/audio/Types.h>

#include <sigc++/sigc++.h>

#include <string>
#include <string_view>
#include <vector>

namespace ao::gtk
{
  class OutputMenuModel final : public sigc::trackable
  {
  public:
    OutputMenuModel() = default;
    ~OutputMenuModel() = default;

    void update(std::vector<ao::audio::IBackendProvider::Status> const& backends,
                ao::audio::BackendId const& currentBackend,
                ao::audio::ProfileId const& currentProfile,
                std::string_view currentDeviceId);

    std::vector<ao::audio::IBackendProvider::Status> const& getBackends() const noexcept { return _backends; }
    ao::audio::BackendId const& getCurrentBackend() const noexcept { return _currentBackend; }
    ao::audio::ProfileId const& getCurrentProfile() const noexcept { return _currentProfile; }
    std::string const& getCurrentDeviceId() const noexcept { return _currentDeviceId; }

    using ChangedSignal = sigc::signal<void()>;
    ChangedSignal& signalChanged() { return _signalChanged; }

  private:
    std::vector<ao::audio::IBackendProvider::Status> _backends;
    ao::audio::BackendId _currentBackend;
    ao::audio::ProfileId _currentProfile;
    std::string _currentDeviceId;

    ChangedSignal _signalChanged;
  };
} // namespace ao::gtk