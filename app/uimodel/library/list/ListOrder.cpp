// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/uimodel/library/list/ListOrder.h>

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/library/LibraryAuthoring.h>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <unordered_set>
#include <vector>

namespace ao::uimodel
{
  ListOrderCapabilityState describeListOrderCapabilities(i18n::MessageCatalog const& textCatalog,
                                                         ListOrderCapabilityInput const& input)
  {
    auto state = ListOrderCapabilityState{};

    if (rt::isVirtualListId(input.listId))
    {
      state.disabledReason = i18n::requiredText(textCatalog, i18n::MessageId::ListOrderSavedListsOnly);
      return state;
    }

    if (input.authoring.state == rt::LibraryAuthoringState::Maintenance)
    {
      state.disabledReason = i18n::requiredText(textCatalog, i18n::MessageId::ListOrderLibraryBusy);
      return state;
    }

    if (input.authoring.state != rt::LibraryAuthoringState::Available)
    {
      state.disabledReason = i18n::requiredText(textCatalog, i18n::MessageId::ListOrderAuthoringUnavailable);
      return state;
    }

    if (!input.sourceLive)
    {
      state.disabledReason = i18n::requiredText(textCatalog, i18n::MessageId::ListOrderListUnavailable);
      return state;
    }

    if (input.sourceHasError)
    {
      state.disabledReason = i18n::requiredText(textCatalog, i18n::MessageId::ListOrderFixFilter);
      return state;
    }

    if (input.presentation.groupBy != rt::TrackGroupKey::None || !input.presentation.sortBy.empty())
    {
      state.disabledReason = i18n::requiredText(textCatalog, i18n::MessageId::ListOrderChooseFlatPresentation);
      return state;
    }

    state.canAuthorOrder = true;
    state.canAbsoluteMove = true;
    state.canResetOrder = true;
    state.canForgetHiddenPositions = true;

    if (input.quickFilterExpression.empty())
    {
      state.canGapMove = true;
      state.canRelativeMove = true;
    }
    else
    {
      state.disabledReason = i18n::requiredText(textCatalog, i18n::MessageId::ListOrderClearQuickFilter);
    }

    return state;
  }

  std::vector<TrackId> listOrderDragSelection(TrackId const draggedTrackId,
                                              std::span<TrackId const> const selectedTrackIds,
                                              std::span<TrackId const> const effectiveTrackIds)
  {
    if (!std::ranges::contains(effectiveTrackIds, draggedTrackId))
    {
      return {};
    }

    auto const selected = std::unordered_set<TrackId>{selectedTrackIds.begin(), selectedTrackIds.end()};

    if (!selected.contains(draggedTrackId))
    {
      return {draggedTrackId};
    }

    auto ordered = std::vector<TrackId>{};
    ordered.reserve(selected.size());

    for (auto const trackId : effectiveTrackIds)
    {
      if (selected.contains(trackId))
      {
        ordered.push_back(trackId);
      }
    }

    return ordered;
  }

  Result<std::optional<TrackId>> listOrderAnchorForGap(std::span<TrackId const> const effectiveTrackIds,
                                                       std::span<TrackId const> const selectedTrackIds,
                                                       std::size_t const gapIndex)
  {
    if (gapIndex > effectiveTrackIds.size())
    {
      return makeError(Error::Code::InvalidInput, "List order drop gap is outside the effective sequence");
    }

    if (std::ranges::contains(effectiveTrackIds, kInvalidTrackId))
    {
      return makeError(Error::Code::InvalidInput, "The effective List sequence contains an invalid track id");
    }

    if (std::ranges::contains(selectedTrackIds, kInvalidTrackId))
    {
      return makeError(Error::Code::InvalidInput, "The dragged selection contains an invalid track id");
    }

    auto const effective = std::unordered_set<TrackId>{effectiveTrackIds.begin(), effectiveTrackIds.end()};
    auto const selected = std::unordered_set<TrackId>{selectedTrackIds.begin(), selectedTrackIds.end()};

    if (selected.empty())
    {
      return makeError(Error::Code::InvalidInput, "List order drop requires at least one selected track");
    }

    for (auto const trackId : selected)
    {
      if (!effective.contains(trackId))
      {
        return makeError(Error::Code::InvalidInput, "Every dragged track must belong to the effective List sequence");
      }
    }

    for (std::size_t index = gapIndex; index < effectiveTrackIds.size(); ++index)
    {
      if (!selected.contains(effectiveTrackIds[index]))
      {
        return std::optional<TrackId>{effectiveTrackIds[index]};
      }
    }

    return std::optional<TrackId>{};
  }
} // namespace ao::uimodel
