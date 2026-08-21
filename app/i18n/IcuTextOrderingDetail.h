// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <unicode/utypes.h>

namespace ao::i18n::detail
{
  Error::Code collationErrorCode(UErrorCode status,
                                 Error::Code successfulStatusFallback = Error::Code::InitFailed) noexcept;
} // namespace ao::i18n::detail
