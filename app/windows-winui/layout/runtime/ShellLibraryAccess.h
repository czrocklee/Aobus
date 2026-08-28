// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Subscription.h>
#include <ao/compat/MoveOnlyFunction.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/uimodel/library/list/ListOrder.h>
#include <ao/uimodel/library/list/ListTreeProjection.h>
#include <ao/uimodel/library/track/TrackAuthoringSessions.h>
#include <ao/winui/list/ListAuthoringAdapter.h>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace ao::winui::layout
{
  /**
   * @brief Narrow library-session capabilities available during shell construction.
   *
   * The owning ShellBuilder stores this object so generation components may
   * retain individual callbacks without borrowing a temporary build context.
   */
  struct ShellLibraryAccess final
  {
    std::filesystem::path libraryRoot;
    std::function<uimodel::ListTreeProjection()> listTreeProjection;
    std::function<async::Subscription(compat::MoveOnlyFunction<void()>)> subscribeListTreeChanged;
    std::function<std::optional<rt::TrackPresentationSpec>(ListId)> preferredPresentation;
    std::function<Result<>(rt::ViewId, TrackId)> playTrack;
    std::function<void(ListId, std::string)> createList;
    std::function<void(ListId)> editList;
    std::function<void(ListId, bool)> deleteList;
    std::function<std::vector<uimodel::WritableTagListTarget>()> membershipTargets;
    std::function<void(ListId, bool)> editMembership;
    std::function<uimodel::ListOrderCapabilityState()> orderCapabilities;
    std::function<void(ListOrderCommand)> applyOrder;
  };
} // namespace ao::winui::layout
