// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "app/UIState.h"

#include <runtime/ConfigStore.h>
#include <runtime/TrackPresentationPreset.h>

#include <sigc++/sigc++.h>

#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace ao::gtk
{
  class TrackPresentationStore final
  {
  public:
    using ChangedSignal = sigc::signal<void()>;

    explicit TrackPresentationStore(std::shared_ptr<rt::ConfigStore> configStore);

    std::span<rt::TrackPresentationPreset const> builtinPresets() const noexcept;
    std::vector<CustomTrackPresentationState> const& customPresentations() const noexcept;

    std::optional<rt::TrackPresentationSpec> specForId(std::string_view id) const;

    void setCustomPresentations(std::vector<CustomTrackPresentationState> presentations);
    void addCustomPresentation(CustomTrackPresentationState const& state);
    void removeCustomPresentation(std::string_view id);

    ChangedSignal& signalChanged() noexcept { return _changed; }

  private:
    void load();
    void save();

    std::shared_ptr<rt::ConfigStore> _configStore;
    TrackPresentationStoreState _state;
    ChangedSignal _changed;
  };

  rt::TrackPresentationSpec specFromState(CustomTrackPresentationState const& state);
} // namespace ao::gtk
