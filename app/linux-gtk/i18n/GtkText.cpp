// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "i18n/GtkText.h"

#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/library/LibraryTransfer.h>

#include <cstddef>
#include <string>
#include <string_view>

namespace ao::gtk
{
  using i18n::MessageId;
  using i18n::requiredFormat;
  using i18n::requiredText;

  std::string deleteListQuestion(i18n::MessageCatalog const& catalog, std::string_view const name)
  {
    return requiredFormat(catalog, MessageId::GtkListDeleteQuestion, {i18n::MessageArgument{"name", name}});
  }

  std::string deleteSubtreeQuestion(i18n::MessageCatalog const& catalog,
                                    std::size_t const count,
                                    std::string_view const entries)
  {
    return requiredFormat(catalog,
                          MessageId::GtkListDeleteSubtreeQuestion,
                          {i18n::MessageArgument{"count", count}, i18n::MessageArgument{"entries", entries}});
  }

  std::string removeMembershipTagQuestion(i18n::MessageCatalog const& catalog,
                                          std::string_view const tag,
                                          std::size_t const count)
  {
    return requiredFormat(
      catalog, MessageId::GtkListRemoveTag, {i18n::MessageArgument{"tag", tag}, i18n::MessageArgument{"count", count}});
  }

  std::string membershipTagReferencesWarning(i18n::MessageCatalog const& catalog,
                                             std::string_view const tag,
                                             std::string_view const references)
  {
    return requiredFormat(catalog,
                          MessageId::GtkListTagReferences,
                          {i18n::MessageArgument{"tag", tag}, i18n::MessageArgument{"references", references}});
  }

  std::string removeFromCurrentList(i18n::MessageCatalog const& catalog,
                                    std::string_view const name,
                                    std::string_view const tag)
  {
    return requiredFormat(catalog,
                          MessageId::GtkListRemoveFromCurrent,
                          {i18n::MessageArgument{"name", name}, i18n::MessageArgument{"tag", tag}});
  }

  std::string libraryRestoreConfirmation(i18n::MessageCatalog const& catalog, rt::ImportReport const& report)
  {
    auto const scopeId = report.targetScope == rt::ImportTargetScope::Library ? MessageId::GtkLibraryRestoreScopeLibrary
                                                                              : MessageId::GtkLibraryRestoreScopeLists;
    return requiredFormat(catalog,
                          MessageId::GtkLibraryRestoreConfirmation,
                          {i18n::MessageArgument{"scope", requiredText(catalog, scopeId)},
                           i18n::MessageArgument{"version", report.payloadVersion},
                           i18n::MessageArgument{"mode", rt::exportModeName(report.payloadMode)},
                           i18n::MessageArgument{"tracksCreated", report.tracksCreated},
                           i18n::MessageArgument{"tracksUpdated", report.tracksUpdated},
                           i18n::MessageArgument{"tracksDeleted", report.tracksDeleted},
                           i18n::MessageArgument{"listsCreated", report.listsCreated},
                           i18n::MessageArgument{"listsDeleted", report.listsDeleted},
                           i18n::MessageArgument{"dangling", report.danglingReferencesIgnored}});
  }

  std::string fileSelectionError(i18n::MessageCatalog const& catalog,
                                 std::string_view const operation,
                                 std::string_view const message)
  {
    return requiredFormat(catalog,
                          MessageId::GtkLibraryFileSelectionError,
                          {i18n::MessageArgument{"operation", operation}, i18n::MessageArgument{"message", message}});
  }
} // namespace ao::gtk
