// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/uimodel/library/list/ListOrderOutcome.h>

#include <ao/Contract.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/library/LibraryAuthoring.h>

#include <cstddef>
#include <string>

namespace ao::uimodel
{
  namespace
  {
    std::string formatOrderOutcome(i18n::MessageCatalog const& textCatalog,
                                   rt::AuthoringStatus const status,
                                   i18n::MessageId const appliedMessageId,
                                   std::size_t const appliedCount)
    {
      switch (status)
      {
        case rt::AuthoringStatus::Applied:
          return i18n::requiredFormat(textCatalog, appliedMessageId, {{"count", appliedCount}});
        case rt::AuthoringStatus::NoOp:
          return std::string{i18n::requiredText(textCatalog, i18n::MessageId::ListOrderUnchanged)};
        case rt::AuthoringStatus::Busy:
          return std::string{i18n::requiredText(textCatalog, i18n::MessageId::ListOrderLibraryBusy)};
        case rt::AuthoringStatus::Stale:
          return std::string{i18n::requiredText(textCatalog, i18n::MessageId::ListOrderChanged)};
        case rt::AuthoringStatus::Unavailable:
          return std::string{i18n::requiredText(textCatalog, i18n::MessageId::ListOrderEditingUnavailable)};
      }

      AO_FATAL("List order command returned an unknown authoring status");
    }
  } // namespace

  std::string formatListOrderOutcome(i18n::MessageCatalog const& textCatalog,
                                     rt::AuthoringResult<rt::MoveListOrderReply> const& outcome)
  {
    return formatOrderOutcome(
      textCatalog, outcome.status, i18n::MessageId::ListOrderMoved, outcome.reply.selectedTrackIds.size());
  }

  std::string formatListOrderOutcome(i18n::MessageCatalog const& textCatalog,
                                     rt::AuthoringResult<rt::ResetListOrderReply> const& outcome)
  {
    return formatOrderOutcome(
      textCatalog, outcome.status, i18n::MessageId::ListOrderReset, outcome.reply.forgottenPositionCount);
  }

  std::string formatListOrderOutcome(i18n::MessageCatalog const& textCatalog,
                                     rt::AuthoringResult<rt::ForgetHiddenListOrderReply> const& outcome)
  {
    return formatOrderOutcome(
      textCatalog, outcome.status, i18n::MessageId::ListOrderForgotHidden, outcome.reply.forgottenPositionCount);
  }
} // namespace ao::uimodel
