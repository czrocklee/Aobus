// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/i18n/MessageCatalog.h>

#include <string_view>

namespace ao::test
{
  /** Catalog fixtures backed by the embedded locale data. */
  i18n::MessageCatalog messageCatalog(std::string_view locale);
  i18n::MessageCatalog const& englishMessageCatalog();
} // namespace ao::test
