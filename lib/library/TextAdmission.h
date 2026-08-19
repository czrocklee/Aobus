// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <cstddef>
#include <string>
#include <string_view>

namespace ao::library::detail
{
  /** Validates external scalar UTF-8 without changing its normalization. */
  Result<> validateLibraryText(std::string_view text, std::string_view context);

  /** Returns the final NFC byte length, avoiding allocation when already NFC. */
  Result<std::size_t> normalizedLibraryTextSize(std::string_view text, std::string_view context);

  /** Validates external text and returns the NFC value admitted to storage. */
  Result<std::string> normalizeLibraryText(std::string_view text, std::string_view context);

  /** Verifies the valid UTF-8 NFC invariant for bytes already held by storage. */
  Result<> validatePersistedLibraryText(std::string_view text, std::string_view context);

  /** Verifies scalar-valid UTF-8 for opaque bytes already held by storage. */
  Result<> validatePersistedLibraryUtf8(std::string_view text, std::string_view context);
} // namespace ao::library::detail
