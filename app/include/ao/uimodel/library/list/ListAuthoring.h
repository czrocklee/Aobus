// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Task.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/ListMutation.h>
#include <ao/uimodel/library/LibraryEditing.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace ao::uimodel
{
  struct ListDeletionLabels final
  {
    i18n::MessageId title = i18n::MessageId::ListDeleteTitle;
    i18n::MessageId action = i18n::MessageId::ListDeleteAction;
  };

  struct ListDeletionCleanupText final
  {
    std::string question{};
    std::string referencesWarning{};
  };

  struct ListDeletionDescription final
  {
    ListDeletionLabels labels{};
    std::string question{};
    std::optional<ListDeletionCleanupText> optCleanup{};
  };

  ListDeletionLabels listDeletionLabels(bool includeDescendants) noexcept;
  std::string formatListDeletionQuestion(i18n::MessageCatalog const& catalog, rt::DeleteListReply const& root);
  std::string formatListSubtreeDeletionQuestion(i18n::MessageCatalog const& catalog,
                                                std::size_t count,
                                                std::string_view entries);
  /// Formats the root tag offer using the caller's already fitted reference names.
  ListDeletionCleanupText describeListDeletionCleanup(i18n::MessageCatalog const& catalog,
                                                      rt::DeleteListReply::TagImpact const& impact,
                                                      std::string_view references);

  /// Fully expanded confirmation: every affected subtree List with its ID and all surviving tag references.
  ListDeletionDescription describeListDeletion(i18n::MessageCatalog const& catalog,
                                               rt::DeleteListSubtreeReply const& preview,
                                               bool includeDescendants);

  /** Narrow List CRUD functions shared by native authoring workflows. */
  async::Task<Result<ListId>> saveListAsync(LibraryEditing editing, rt::ListDraft draft);
  async::Task<Result<rt::DeleteListSubtreeReply>> previewListDeletionAsync(LibraryEditing editing,
                                                                           ListId listId,
                                                                           bool includeDescendants);
  async::Task<Result<rt::DeleteListSubtreeReply>> deleteListAsync(LibraryEditing editing,
                                                                  ListId listId,
                                                                  bool includeDescendants,
                                                                  rt::DeleteListOptions options = {});
} // namespace ao::uimodel
