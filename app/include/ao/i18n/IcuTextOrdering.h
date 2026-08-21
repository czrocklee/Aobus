// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/rt/ordering/TextOrderingPolicy.h>

#include <memory>
#include <string_view>

namespace ao::i18n
{
  /** Constructs the governed ICU collation candidate for one BCP 47 locale. */
  Result<std::unique_ptr<rt::TextOrderingPolicy>> createIcuTextOrderingPolicy(std::string_view localeTag);
} // namespace ao::i18n
