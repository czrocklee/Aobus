// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "i18n/GtkText.h"

#include <ao/i18n/MessageCatalog.h>

#include <string>
#include <string_view>

namespace ao::gtk
{
  using i18n::MessageId;
  using i18n::requiredFormat;

  std::string fileSelectionError(i18n::MessageCatalog const& catalog,
                                 std::string_view const operation,
                                 std::string_view const message)
  {
    return requiredFormat(catalog,
                          MessageId::LibraryFileSelectionError,
                          {i18n::MessageArgument{"operation", operation}, i18n::MessageArgument{"message", message}});
  }
} // namespace ao::gtk
