// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/library/LibraryAuthoring.h>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ao::i18n
{
  class MessageCatalog;
}

namespace ao::uimodel
{
  struct ListOrderCapabilityInput final
  {
    ListId listId = kInvalidListId;
    rt::TrackPresentationSpec presentation{};
    std::string_view quickFilterExpression{};
    bool sourceLive = true;
    bool sourceHasError = false;
    rt::LibraryAuthoringAvailability authoring{};
  };

  struct ListOrderCapabilityState final
  {
    bool canAuthorOrder = false;
    bool canGapMove = false;
    bool canRelativeMove = false;
    bool canAbsoluteMove = false;
    bool canResetOrder = false;
    bool canForgetHiddenPositions = false;
    std::string disabledReason{};

    bool operator==(ListOrderCapabilityState const&) const = default;
  };

  ListOrderCapabilityState describeListOrderCapabilities(i18n::MessageCatalog const& textCatalog,
                                                         ListOrderCapabilityInput const& input);

  std::vector<TrackId> listOrderDragSelection(TrackId draggedTrackId,
                                              std::span<TrackId const> selectedTrackIds,
                                              std::span<TrackId const> effectiveTrackIds);
  Result<std::optional<TrackId>> listOrderAnchorForGap(std::span<TrackId const> effectiveTrackIds,
                                                       std::span<TrackId const> selectedTrackIds,
                                                       std::size_t gapIndex);
} // namespace ao::uimodel
