// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/async/Subscription.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/OutputDeviceSelection.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/PlaybackState.h>
#include <ao/rt/playback/PlaybackCommands.h>
#include <ao/uimodel/playback/output/OutputDeviceIntent.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace ao::rt
{
  class PlaybackService;
  struct PlaybackSnapshot;
}

namespace ao::uimodel
{
  struct OutputDeviceRow final
  {
    enum class Kind : std::uint8_t
    {
      BackendHeader,
      DeviceProfile,
      Status,        // Informational; not selectable.
      RoutingPolicy, // Toggles consent for optRoutingPolicySelection, not an output selection.
    };

    Kind kind = Kind::BackendHeader;
    audio::BackendId backendId{};
    audio::DeviceId deviceId{};
    audio::ProfileId profileId{};
    std::string title{};
    std::string description{};
    // Selected/preferred route for a DeviceProfile; consent for RoutingPolicy. Not delivery evidence.
    bool isActive = false;
    bool isExclusive = false;
    std::optional<audio::OutputDeviceSelection> optRoutingPolicySelection{};
  };

  struct OutputDeviceViewState final
  {
    std::vector<OutputDeviceRow> rows{};
    std::string outputBackendSummary{};
    std::string outputDeviceStatus{};
    // A route is configured; the physical output may differ when system rerouting is allowed.
    bool hasActiveOutputDevice = false;
  };

  class OutputDeviceViewModel final
  {
  public:
    using RenderCallback = std::function<void(OutputDeviceViewState const&)>;

    /// @param intent Where a requested route is recorded; `OutputDeviceIntent::discarded()` keeps none.
    OutputDeviceViewModel(rt::PlaybackService& playback,
                          i18n::MessageCatalog textCatalog,
                          RenderCallback onRender,
                          OutputDeviceIntent intent);

    OutputDeviceViewModel(OutputDeviceViewModel const&) = delete;
    OutputDeviceViewModel& operator=(OutputDeviceViewModel const&) = delete;
    OutputDeviceViewModel(OutputDeviceViewModel&&) = delete;
    OutputDeviceViewModel& operator=(OutputDeviceViewModel&&) = delete;

    ~OutputDeviceViewModel() = default;

    void selectOutputDevice(audio::BackendId const& backendId,
                            audio::DeviceId const& deviceId,
                            audio::ProfileId const& profileId);

    /// Toggles consent only if the request captured by the initiating row is still current.
    bool tryToggleOutputRoutingPolicy(audio::OutputDeviceSelection const& expected);

    void refresh();

    /// Stops automatic Playback snapshot observation immediately; calling twice is harmless.
    /// Observation cannot be restarted for this instance.
    /// Explicit refresh, commands, and setTextCatalog() still require a live PlaybackService.
    void stopObserving();

    void setTextCatalog(i18n::MessageCatalog textCatalog);

  private:
    void submitOutputDevice(audio::OutputDeviceSelection selection);
    void handleSnapshot(rt::PlaybackSnapshot const& snapshot);
    void render(rt::OutputState const& output);

    rt::PlaybackService& _playback;
    rt::PlaybackCommands& _commands;
    RenderCallback _onRender;
    OutputDeviceIntent _intent;
    i18n::MessageCatalog _textCatalog;
    rt::OutputState _lastOutput{};

    async::Subscription _snapshotSub;
  };
} // namespace ao::uimodel
