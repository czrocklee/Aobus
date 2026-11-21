// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/i18n/MessageCatalog.h>

#include <string>
#include <string_view>

namespace ao::gtk
{
  /// Owning GTK boundary adapter for an argument-free message; invalid selections fail closed.
  inline std::string gtkText(i18n::MessageCatalog const& catalog, i18n::MessageId const id)
  {
    return std::string{i18n::requiredText(catalog, id)};
  }

  std::string fileSelectionError(i18n::MessageCatalog const& catalog,
                                 std::string_view operation,
                                 std::string_view message);
} // namespace ao::gtk
