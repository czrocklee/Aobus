// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/i18n/MessageCatalog.h>

#include <string_view>

namespace ao::rt
{
  class NotificationService;
}

namespace ao::uimodel
{
  /**
   * @brief Tells the user that protected configuration was saved by a newer version of Aobus (decision 0020).
   *
   * This build ignores that configuration and every save of it fails, so the
   * notice says so when the configuration loads. A failed save adds no
   * further report, unless the command that saved reports its own failure,
   * as a shortcut edit does. The warning is retained in history. @p member names the
   * configuration and keys the notice, so a later load of the same member
   * replaces its notice rather than adding another.
   */
  void reportNewerConfig(rt::NotificationService& notifications,
                         i18n::MessageCatalog const& catalog,
                         std::string_view member,
                         i18n::MessageId message);
} // namespace ao::uimodel
