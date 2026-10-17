// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/i18n/MessageCatalog.h>

#include <string>

namespace ao::i18n::detail
{
  Result<std::string> systemLocaleTag();

  // The operating system may report a language that strict admission rejects;
  // startup then uses English, as it does when no language is reported.
  Result<MessageCatalog> createCatalogForSystemLocale(Result<std::string> const& systemLocaleRes);
} // namespace ao::i18n::detail
