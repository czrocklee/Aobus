// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <cstdint>

namespace ao::library
{
  /// Controls schema creation on library open, not the permissions of the environment.
  enum class LibrarySchemaPolicy : std::uint8_t
  {
    InitializeIfEmpty,
    /// Reject missing or zero-length data before opening storage. A nonempty
    /// data file still uses normal writable locking and schema validation.
    RequireExisting,
  };
} // namespace ao::library
