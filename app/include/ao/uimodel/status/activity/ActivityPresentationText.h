// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/NotificationState.h>
#include <ao/rt/library/LibraryTaskEvents.h>
#include <ao/uimodel/library/task/LibraryScanOutcome.h>

#include <cstddef>
#include <string>
#include <string_view>

namespace ao
{
  struct Error;
}

namespace ao::i18n
{
  class MessageCatalog;
}

namespace ao::uimodel
{
  std::string notificationMessage(i18n::MessageCatalog const& catalog, rt::NotificationMessage const& message);
  std::string notificationGroupMessage(i18n::MessageCatalog const& catalog,
                                       rt::NotificationSeverity severity,
                                       std::size_t count);
  std::string libraryTaskProgressDetail(i18n::MessageCatalog const& catalog,
                                        rt::LibraryTaskProgressKind kind,
                                        std::string_view subject);
  std::string libraryTaskProgressCompact(i18n::MessageCatalog const& catalog,
                                         rt::LibraryTaskProgressKind kind,
                                         std::string_view subject);
  std::string formatLibraryScanMessage(i18n::MessageCatalog const& catalog, LibraryScanOutcome const& outcome);

  /**
   * @brief Builds the report for a library switch whose new library could not be saved as the startup library.
   *
   * The switched-to library stays open and usable, so this is a warning rather
   * than a startup error. Its consequence appears only at the next launch,
   * which reopens the previous library, so the report is retained in history
   * instead of expiring before the user can read it.
   */
  rt::NotificationRequest librarySwitchNotSavedNotification(i18n::MessageCatalog const& catalog, Error const& error);
} // namespace ao::uimodel
