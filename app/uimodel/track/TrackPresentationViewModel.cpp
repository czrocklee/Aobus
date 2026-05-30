// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/Type.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/uimodel/track/TrackPresentationRecommender.h>
#include <ao/uimodel/track/TrackPresentationViewModel.h>

#include <algorithm>
#include <map>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::uimodel::track
{
  TrackPresentationViewModel::TrackPresentationViewModel(rt::WorkspaceService& workspace)
    : _workspace{workspace}
  {
    _customPresetsSub = _workspace.onCustomPresetsChanged(
      [this] { _changed.emit(ao::kInvalidListId, TrackPresentationChangeType::FullRebuild); });
  }

  std::span<rt::TrackPresentationPreset const> TrackPresentationViewModel::builtinPresets() const noexcept
  {
    return rt::builtinTrackPresentationPresets();
  }

  std::span<rt::CustomTrackPresentationPreset const> TrackPresentationViewModel::customPresentations() const noexcept
  {
    return _workspace.customPresets();
  }

  std::optional<rt::TrackPresentationSpec> TrackPresentationViewModel::specForId(std::string_view id) const
  {
    if (auto const* builtin = rt::builtinTrackPresentationPreset(id); builtin != nullptr)
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

  void TrackPresentationViewModel::setActivePresentationId(std::string_view id)
  {
    if (_activePresentationId == id)
    {
      return;
    }

    _activePresentationId = id;

    _changed.emit(_activeListId, TrackPresentationChangeType::LayoutOnly);
  }

  std::vector<ColumnState> const& TrackPresentationViewModel::layoutForList(ao::ListId listId) const noexcept
  {
    static std::vector<ColumnState> const kEmpty{};

    if (auto const it = _listLayouts.find(listId); it != _listLayouts.end())
    {
      return it->second;
    }

    return kEmpty;
  }

  void TrackPresentationViewModel::updateLayout(ao::ListId listId, std::vector<ColumnState> const& layout)
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

  std::vector<rt::TrackField> TrackPresentationViewModel::activeFieldOrder() const noexcept
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

  void TrackPresentationViewModel::setActiveListId(ao::ListId listId)
  {
    if (_activeListId == listId)
    {
      return;
    }

    _activeListId = listId;
    _changed.emit(_activeListId, TrackPresentationChangeType::LayoutOnly);
  }

  void TrackPresentationViewModel::setListLayouts(std::map<ao::ListId, std::vector<ColumnState>> const& layouts)
  {
    if (_listLayouts == layouts)
    {
      return;
    }

    _listLayouts = layouts;
    _changed.emit(ao::kInvalidListId, TrackPresentationChangeType::LayoutOnly);
  }

  void TrackPresentationViewModel::setListPresentations(std::map<ao::ListId, std::string> const& presentations)
  {
    if (_presentations == presentations)
    {
      return;
    }

    _presentations = presentations;
    _changed.emit(ao::kInvalidListId, TrackPresentationChangeType::FullRebuild);
  }

  std::optional<std::string_view> TrackPresentationViewModel::presentationIdForList(ao::ListId listId) const
  {
    if (auto const it = _presentations.find(listId); it != _presentations.end())
    {
      return it->second;
    }

    return std::nullopt;
  }

  void TrackPresentationViewModel::setPresentationIdForList(ao::ListId listId, std::string_view presentationId)
  {
    if (listId == ao::kInvalidListId)
    {
      return;
    }

    auto const strId = std::string{presentationId};

    if (_presentations[listId] == strId)
    {
      return;
    }

    _presentations[listId] = strId;
    _changed.emit(listId, TrackPresentationChangeType::FullRebuild);
  }

  void TrackPresentationViewModel::clearPresentationForList(ao::ListId listId)
  {
    if (_presentations.erase(listId) > 0)
    {
      _changed.emit(listId, TrackPresentationChangeType::FullRebuild);
    }
  }

  rt::TrackPresentationSpec TrackPresentationViewModel::presentationForList(ao::ListId listId,
                                                                            std::string_view smartListFilter) const
  {
    if (auto const optId = presentationIdForList(listId); optId)
    {
      if (auto const optSpec = specForId(*optId); optSpec)
      {
        return *optSpec;
      }
    }

    return recommendPresentation(smartListFilter, builtinPresets(), customPresentations());
  }

  void TrackPresentationViewModel::addCustomPresentation(rt::CustomTrackPresentationPreset const& state)
  {
    _workspace.addCustomPreset(state);
  }

  void TrackPresentationViewModel::removeCustomPresentation(std::string_view id)
  {
    _workspace.removeCustomPreset(id);
  }
} // namespace ao::uimodel::track
