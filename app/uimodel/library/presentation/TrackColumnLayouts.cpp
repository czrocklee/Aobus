// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/presentation/TrackColumnLayouts.h>

#include "RetainKnownLists.h"
#include <ao/CoreIds.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/library/LibraryChanges.h>

#include <algorithm>
#include <map>
#include <span>
#include <utility>
#include <vector>

namespace ao::uimodel
{
  namespace
  {
    bool matchesGroupHeading(rt::TrackGroupKey const key, rt::TrackField const field)
    {
      // A credit heading gives one name, not the row's additional-credit count.
      if (key == rt::TrackGroupKey::None || rt::creditKindForTrackField(field))
      {
        return false;
      }

      // TrackListProjection builds Album and Work headings around compound
      // group identities. Only equivalent row facts qualify, not Album's
      // fallback artist or representative year.
      auto const* definition = rt::trackFieldDefinition(field);
      return (definition != nullptr && definition->optGroupKey == key) ||
             (key == rt::TrackGroupKey::Album && field == rt::TrackField::AlbumArtist) ||
             (key == rt::TrackGroupKey::Work && field == rt::TrackField::Composer);
    }
  } // namespace

  TrackColumnLayouts::TrackColumnLayouts(rt::LibraryChanges const& changes)
  {
    _changesSubscription = changes.onChanged(
      [this](rt::LibraryChangeSet const& changeSet)
      {
        auto const removedListIds = pruneChangedLists(_listLayouts, changeSet);

        // An observer may destroy this owner; retain the signal for the whole batch.
        auto const changedPtr = _changedPtr;

        for (auto const listId : removedListIds)
        {
          changedPtr->emit(listId);
        }
      });
  }

  TrackColumnLayouts::~TrackColumnLayouts() = default;

  void TrackColumnLayouts::restore(Snapshot layouts, std::span<ListId const> const knownListIds)
  {
    retainKnownLists(layouts, knownListIds);

    if (_listLayouts == layouts)
    {
      return;
    }

    _listLayouts = std::move(layouts);
    _changedPtr->emit(kInvalidListId);
  }

  std::vector<TrackColumnState> const& TrackColumnLayouts::layoutForList(ListId listId) const noexcept
  {
    static std::vector<TrackColumnState> const kEmpty{};

    if (auto const it = _listLayouts.find(listId); it != _listLayouts.end())
    {
      return it->second;
    }

    return kEmpty;
  }

  void TrackColumnLayouts::updateLayout(ListId listId, std::vector<TrackColumnState> const& layout)
  {
    if (listId == kInvalidListId)
    {
      return;
    }

    // An absent entry and an empty layout are the same state to every reader,
    // so neither direction between them is a change. find() rather than
    // operator[]: comparing through the subscript maps the id as a side effect,
    // and the persisting observer writes whatever the map holds.
    if (auto const it = _listLayouts.find(listId); it == _listLayouts.end() ? layout.empty() : it->second == layout)
    {
      return;
    }

    _listLayouts[listId] = layout;
    _changedPtr->emit(listId);
  }

  std::vector<rt::TrackField> visibleTrackFieldsInStoredOrder(std::span<rt::TrackField const> const visibleFields,
                                                              std::span<rt::TrackField const> const storedOrder)
  {
    auto ordered = std::vector<rt::TrackField>{};
    ordered.reserve(visibleFields.size());

    auto const appendIfVisible = [&ordered, visibleFields](rt::TrackField const field)
    {
      if (!std::ranges::contains(visibleFields, field) || std::ranges::contains(ordered, field))
      {
        return;
      }

      ordered.push_back(field);
    };

    for (auto const field : storedOrder)
    {
      appendIfVisible(field);
    }

    for (auto const field : visibleFields)
    {
      appendIfVisible(field);
    }

    return ordered;
  }

  std::vector<TrackColumnChoice> trackColumnChoices(rt::TrackPresentationSpec const& presentation,
                                                    std::span<TrackColumnState const> const storedLayout)
  {
    auto choices = std::vector<TrackColumnChoice>{};
    choices.reserve(presentation.visibleFields.size() + presentation.redundantFields.size());
    auto const appendChoice = [&choices, storedLayout](rt::TrackField const field, bool const defaultVisible)
    {
      if (std::ranges::contains(choices, field, &TrackColumnChoice::field))
      {
        return;
      }

      auto const stored = std::ranges::find(storedLayout, field, &TrackColumnState::field);
      auto const visible = stored == storedLayout.end() || stored->visibility == TrackColumnVisibility::Default
                             ? defaultVisible
                             : stored->visibility == TrackColumnVisibility::Show;
      choices.push_back({.field = field, .visible = visible});
    };

    // The authored list is authoritative, including a field that also appears in the heading.
    for (auto const field : presentation.visibleFields)
    {
      appendChoice(field, true);
    }

    for (auto const field : presentation.redundantFields)
    {
      if (matchesGroupHeading(presentation.groupBy, field))
      {
        appendChoice(field, false);
      }
    }

    return choices;
  }

  std::vector<rt::TrackField> visibleTrackFieldsInStoredLayout(rt::TrackPresentationSpec const& presentation,
                                                               std::span<TrackColumnState const> const storedLayout)
  {
    auto const choices = trackColumnChoices(presentation, storedLayout);
    auto enabled = std::vector<rt::TrackField>{};
    enabled.reserve(choices.size());

    for (auto const choice : choices)
    {
      if (choice.visible)
      {
        enabled.push_back(choice.field);
      }
    }

    auto order = std::vector<rt::TrackField>{};
    order.reserve(storedLayout.size());

    for (auto const& column : storedLayout)
    {
      if (column.hasPosition())
      {
        order.push_back(column.field);
      }
    }

    return visibleTrackFieldsInStoredOrder(enabled, order);
  }
} // namespace ao::uimodel
