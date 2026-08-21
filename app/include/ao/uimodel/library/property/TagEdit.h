// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/rt/library/LibraryAuthoring.h>

#include <span>
#include <string>

namespace ao::uimodel
{
  class TrackAuthoringSession;
  class PresentationTextCatalog;

  struct TagEditResult final
  {
    rt::TrackAuthoringStatus status = rt::TrackAuthoringStatus::NoOp;
    std::string notificationText;
  };

  Result<TagEditResult> applyTagEdit(TrackAuthoringSession& session,
                                     PresentationTextCatalog const& textCatalog,
                                     std::span<std::string const> tagsToAdd,
                                     std::span<std::string const> tagsToRemove);
} // namespace ao::uimodel
