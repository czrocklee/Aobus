// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/async/Task.h>
#include <ao/rt/library/LibraryAuthoring.h>

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
  class TrackAuthoringSession;

  struct CommonTagChanges final
  {
    std::vector<std::string> tagsToAdd{};
    std::vector<std::string> tagsToRemove{};

    bool empty() const noexcept { return tagsToAdd.empty() && tagsToRemove.empty(); }
  };

  /// Admit a token into a common-tag draft; false means its NFC spelling already exists.
  /// Invalid input leaves the draft unchanged. The caller owns whitespace/token boundaries.
  Result<bool> appendCommonTag(std::vector<std::string>& tags, std::string_view candidate);

  /// Compare the selection's stored common tags with its desired common tags, in display order.
  /// Both inputs are NFC sets (use appendCommonTag for user input). Partial memberships not
  /// represented in the baseline are never removals.
  CommonTagChanges diffCommonTags(std::span<std::string const> baseline, std::span<std::string const> current);

  struct TagEditResult final
  {
    rt::AuthoringStatus status = rt::AuthoringStatus::NoOp;
    std::string notificationText;
  };

  async::Task<Result<TagEditResult>> applyTagEditAsync(TrackAuthoringSession& session,
                                                       i18n::MessageCatalog const& textCatalog,
                                                       std::vector<std::string> tagsToAdd,
                                                       std::vector<std::string> tagsToRemove);
} // namespace ao::uimodel
