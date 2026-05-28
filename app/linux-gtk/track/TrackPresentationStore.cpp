// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackPresentationStore.h"

#include "app/UIState.h"
#include <ao/Type.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/WorkspaceService.h>

#include <algorithm>
#include <map>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::gtk
{
  TrackPresentationStore::TrackPresentationStore(rt::WorkspaceService& workspace)
    : _workspace{workspace}
  {
    _customPresetsSub = _workspace.onCustomPresetsChanged(
      [this] { _changed.emit(ao::kInvalidListId, TrackPresentationChangeType::FullRebuild); });
  }

  std::span<rt::TrackPresentationPreset const> TrackPresentationStore::builtinPresets() const noexcept
  {
    return rt::builtinTrackPresentationPresets();
  }

  std::span<rt::CustomTrackPresentationPreset const> TrackPresentationStore::customPresentations() const noexcept
  {
    return _workspace.customPresets();
  }

  std::optional<rt::TrackPresentationSpec> TrackPresentationStore::specForId(std::string_view id) const
  {
    if (auto const* builtin = rt::builtinTrackPresentationPreset(id))
    {
      return builtin->spec;
    }

    auto const presets = _workspace.customPresets();
    auto const it = std::ranges::find(presets, id, [](auto const& preset) { return preset.spec.id; });

    if (it != presets.end())
    {
      return it->spec;
    }

    return std::nullopt;
  }

  void TrackPresentationStore::setActivePresentationId(std::string_view id)
  {
    if (_activePresentationId == id)
    {
      return;
    }

    _activePresentationId = id;

    _changed.emit(_activeListId, TrackPresentationChangeType::LayoutOnly);
  }

  std::vector<ColumnState> const& TrackPresentationStore::layoutForList(ao::ListId listId) const noexcept
  {
    static std::vector<ColumnState> const kEmpty{};

    if (auto const it = _listLayouts.find(listId); it != _listLayouts.end())
    {
      return it->second;
    }

    return kEmpty;
  }

  void TrackPresentationStore::updateLayout(ao::ListId listId, std::vector<ColumnState> const& layout)
  {
    if (listId == ao::kInvalidListId)
    {
      return;
    }

    if (_listLayouts[listId] == layout)
    {
      return;
    }

    _listLayouts[listId] = layout;
    _changed.emit(listId, TrackPresentationChangeType::LayoutOnly);
  }

  std::vector<rt::TrackField> TrackPresentationStore::activeFieldOrder() const noexcept
  {
    auto const& layout = layoutForList(_activeListId);
    auto order = std::vector<rt::TrackField>{};
    order.reserve(layout.size());

    for (auto const& col : layout)
    {
      order.push_back(col.field);
    }

    return order;
  }

  void TrackPresentationStore::setActiveListId(ao::ListId listId)
  {
    if (_activeListId == listId)
    {
      return;
    }

    _activeListId = listId;
    _changed.emit(_activeListId, TrackPresentationChangeType::LayoutOnly);
  }

  void TrackPresentationStore::setListLayouts(std::map<ao::ListId, std::vector<ColumnState>> const& layouts)
  {
    if (_listLayouts == layouts)
    {
      return;
    }

    _listLayouts = layouts;
    _changed.emit(ao::kInvalidListId, TrackPresentationChangeType::LayoutOnly);
  }

  void TrackPresentationStore::addCustomPresentation(rt::CustomTrackPresentationPreset const& state)
  {
    _workspace.addCustomPreset(state);
  }

  void TrackPresentationStore::removeCustomPresentation(std::string_view id)
  {
    _workspace.removeCustomPreset(id);
  }
} // namespace ao::gtk
