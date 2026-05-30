// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Type.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>

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

namespace ao::uimodel::track
{
  enum class TrackPresentationChangeType : std::uint8_t
  {
    FullRebuild,
    LayoutOnly,
  };

  struct ColumnState final
  {
    rt::TrackField field = rt::TrackField::Title;
    std::int32_t width = -1;

    bool operator==(ColumnState const&) const = default;
  };

  struct TrackColumnLayoutState final
  {
    std::map<ListId, std::vector<ColumnState>> listLayouts;
  };

  struct ListPresentationPreferenceState final
  {
    std::map<ListId, std::string> presentations;
  };

  class TrackPresentationViewModel final
  {
  public:
    explicit TrackPresentationViewModel(rt::WorkspaceService& workspace);

    std::span<rt::TrackPresentationPreset const> builtinPresets() const noexcept;
    std::span<rt::CustomTrackPresentationPreset const> customPresentations() const noexcept;

    std::optional<rt::TrackPresentationSpec> specForId(std::string_view id) const;

    void addCustomPresentation(rt::CustomTrackPresentationPreset const& state);
    void removeCustomPresentation(std::string_view id);

    // Active Layout Management
    void setActivePresentationId(std::string_view id);
    std::string_view activePresentationId() const noexcept { return _activePresentationId; }

    void setActiveListId(ListId listId);

    std::map<ListId, std::vector<ColumnState>> const& listLayouts() const noexcept { return _listLayouts; }
    void setListLayouts(std::map<ListId, std::vector<ColumnState>> const& layouts);

    std::vector<ColumnState> const& layoutForList(ListId listId) const noexcept;
    void updateLayout(ListId listId, std::vector<ColumnState> const& layout);

    std::map<ListId, std::string> const& listPresentations() const noexcept { return _presentations; }
    void setListPresentations(std::map<ListId, std::string> const& presentations);

    std::optional<std::string_view> presentationIdForList(ListId listId) const;
    void setPresentationIdForList(ListId listId, std::string_view presentationId);
    void clearPresentationForList(ListId listId);
    rt::TrackPresentationSpec presentationForList(ListId listId, std::string_view smartListFilter = "") const;

    std::vector<rt::TrackField> activeFieldOrder() const noexcept;

    rt::Signal<ListId, TrackPresentationChangeType>& signalChanged() noexcept { return _changed; }

  private:
    rt::WorkspaceService& _workspace;
    rt::Subscription _customPresetsSub;

    std::string _activePresentationId{};
    ListId _activeListId = kInvalidListId;
    std::map<ListId, std::vector<ColumnState>> _listLayouts{};
    std::map<ListId, std::string> _presentations{};
    rt::Signal<ListId, TrackPresentationChangeType> _changed;
  };
} // namespace ao::uimodel::track
