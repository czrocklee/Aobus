// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/async/Task.h>
#include <ao/rt/library/LibraryAuthoring.h>

#include <string>
#include <vector>

namespace ao::uimodel
{
  class TrackAuthoringSession;
  class PresentationTextCatalog;

  struct TagEditResult final
  {
    rt::AuthoringStatus status = rt::AuthoringStatus::NoOp;
    std::string notificationText;
  };

  async::Task<Result<TagEditResult>> applyTagEdit(TrackAuthoringSession& session,
                                                  PresentationTextCatalog const& textCatalog,
                                                  std::vector<std::string> tagsToAdd,
                                                  std::vector<std::string> tagsToRemove);
} // namespace ao::uimodel
