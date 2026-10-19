// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/uimodel/preference/NewerConfigNotice.h>

#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/NotificationIds.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>

#include <format>
#include <string>
#include <string_view>

namespace ao::uimodel
{
  void reportNewerConfig(rt::NotificationService& notifications,
                         i18n::MessageCatalog const& catalog,
                         std::string_view const member,
                         i18n::MessageId const message)
  {
    notifications.createOrUpdate(rt::NotificationReportKey{std::format("config.newer.{}", member)},
                                 rt::NotificationRequest{
                                   .severity = rt::NotificationSeverity::Warning,
                                   .message = std::string{i18n::requiredText(catalog, message)},
                                   .lifetime = rt::NotificationLifetime::history(),
                                 });
  }
} // namespace ao::uimodel
