// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <string>

namespace ao::i18n
{
  class MessageCatalog;
}

namespace ao::rt
{
  template<typename Reply>
  struct AuthoringResult;

  struct ForgetHiddenListOrderReply;
  struct MoveListOrderReply;
  struct ResetListOrderReply;
}

namespace ao::uimodel
{
  /// Formats a writer status and its action-specific count, not an operational error.
  std::string formatListOrderOutcome(i18n::MessageCatalog const& textCatalog,
                                     rt::AuthoringResult<rt::MoveListOrderReply> const& outcome);

  std::string formatListOrderOutcome(i18n::MessageCatalog const& textCatalog,
                                     rt::AuthoringResult<rt::ResetListOrderReply> const& outcome);

  std::string formatListOrderOutcome(i18n::MessageCatalog const& textCatalog,
                                     rt::AuthoringResult<rt::ForgetHiddenListOrderReply> const& outcome);
} // namespace ao::uimodel
