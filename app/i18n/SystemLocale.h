// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <string>

namespace ao::i18n::detail
{
  Result<std::string> systemLocaleTag();
} // namespace ao::i18n::detail
