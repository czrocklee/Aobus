// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "ao/Type.h"
#include "app/UIState.h"
#include "runtime/CorePrimitives.h"
#include "runtime/TrackField.h"
#include "runtime/TrackPresentation.h"

#include <sigc++/signal.h>

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ao::rt
{
  class WorkspaceService;
}

namespace ao::gtk
{
  enum class TrackPresentationChangeType : std::uint8_t
  {
    FullRebuild,
    LayoutOnly,
  };

  class TrackPresentationStore final
  {
  public:
    using ChangedSignal = sigc::signal<void(ao::ListId, TrackPresentationChangeType)>;

    explicit TrackPresentationStore(rt::WorkspaceService& workspace);

    std::span<rt::TrackPresentationPreset const> builtinPresets() const noexcept;
    std::span<rt::CustomTrackPresentationPreset const> customPresentations() const noexcept;

    std::optional<rt::TrackPresentationSpec> specForId(std::string_view id) const;

    void addCustomPresentation(rt::CustomTrackPresentationPreset const& state);
    void removeCustomPresentation(std::string_view id);

    // Active Layout Management
    void setActivePresentationId(std::string_view id);
    std::string_view activePresentationId() const noexcept { return _activePresentationId; }

    void setActiveListId(ao::ListId listId);

    std::map<ao::ListId, std::vector<ColumnState>> const& listLayouts() const noexcept { return _listLayouts; }
    void setListLayouts(std::map<ao::ListId, std::vector<ColumnState>> const& layouts);

    std::vector<ColumnState> const& layoutForList(ao::ListId listId) const noexcept;
    void updateLayout(ao::ListId listId, std::vector<ColumnState> const& layout);

    std::vector<rt::TrackField> activeFieldOrder() const noexcept;

    ChangedSignal& signalChanged() noexcept { return _changed; }

  private:
    rt::WorkspaceService& _workspace;
    rt::Subscription _customPresetsSub;

    std::string _activePresentationId{};
    ao::ListId _activeListId = ao::kInvalidListId;
    std::map<ao::ListId, std::vector<ColumnState>> _listLayouts{};
    ChangedSignal _changed;
  };
} // namespace ao::gtk
