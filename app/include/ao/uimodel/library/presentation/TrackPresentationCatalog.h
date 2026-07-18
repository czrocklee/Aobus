// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/async/Signal.h>
#include <ao/async/Subscription.h>
#include <ao/rt/TrackPresentation.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ao::rt
{
  class WorkspaceService;
}

namespace ao::uimodel
{
  enum class TrackPresentationMenuItemType : std::uint8_t
  {
    Preset,
    Separator,
    CreateCustomView,
  };

  struct TrackPresentationMenuItem final
  {
    TrackPresentationMenuItemType type = TrackPresentationMenuItemType::Preset;
    std::string id = {};
    std::string label = {};

    bool operator==(TrackPresentationMenuItem const&) const = default;
  };

  class TrackPresentationCatalog final
  {
  public:
    explicit TrackPresentationCatalog(rt::WorkspaceService& workspace);

    std::span<rt::TrackPresentationPreset const> builtinPresets() const noexcept;
    std::span<rt::CustomTrackPresentationPreset const> customPresentations() const noexcept;

    std::optional<rt::TrackPresentationSpec> specForId(std::string_view id) const;
    std::string labelForId(std::string_view id) const;
    std::vector<TrackPresentationMenuItem> menuItems() const;

    void addCustomPresentation(rt::CustomTrackPresentationPreset const& state);
    void removeCustomPresentation(std::string_view id);

    async::Signal<>& signalChanged() noexcept { return _changed; }

  private:
    rt::CustomTrackPresentationPreset const* findCustomPresetById(std::string_view id) const;

    rt::WorkspaceService& _workspace;
    async::Subscription _customPresetsSub;
    async::Signal<> _changed;
  };
} // namespace ao::uimodel
