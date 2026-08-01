// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <cstddef>
#include <span>
#include <string_view>

namespace ao::library
{
  struct ValidatedFileManifestEntry final
  {
    std::string_view uri;
  };

  Result<> validateFileManifestPayload(std::span<std::byte const> payload);
  Result<ValidatedFileManifestEntry> validateFileManifestEntry(std::span<std::byte const> rawKey,
                                                               std::span<std::byte const> payload);
} // namespace ao::library
