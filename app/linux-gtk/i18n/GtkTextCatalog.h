// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/i18n/MessageCatalog.h>

#include <cstddef>
#include <string>
#include <string_view>

namespace ao::rt
{
  struct ImportReport;
}

namespace ao::gtk
{
  /// Owning GTK boundary adapter for an argument-free message; invalid selections fail closed.
  inline std::string gtkText(i18n::MessageCatalog const& catalog, i18n::MessageId const id)
  {
    return std::string{i18n::requiredText(catalog, id)};
  }

  std::string deleteListQuestion(i18n::MessageCatalog const& catalog, std::string_view name);
  std::string deleteSubtreeQuestion(i18n::MessageCatalog const& catalog, std::size_t count, std::string_view entries);
  std::string removeMembershipTagQuestion(i18n::MessageCatalog const& catalog, std::string_view tag, std::size_t count);
  std::string membershipTagReferencesWarning(i18n::MessageCatalog const& catalog,
                                             std::string_view tag,
                                             std::string_view references);
  std::string removeFromCurrentList(i18n::MessageCatalog const& catalog, std::string_view name, std::string_view tag);
  std::string libraryRestoreConfirmation(i18n::MessageCatalog const& catalog, rt::ImportReport const& report);
  std::string fileSelectionError(i18n::MessageCatalog const& catalog,
                                 std::string_view operation,
                                 std::string_view message);
} // namespace ao::gtk
