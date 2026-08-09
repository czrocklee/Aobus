// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/library/LibraryUri.h>

#include <array>
#include <cstddef>
#include <span>
#include <string_view>

namespace ao::library
{
  namespace detail
  {
    /** Fixed-capacity zero-padded manifest key storage; callers establish URI canonicality. */
    class PaddedFileManifestKey final
    {
    public:
      explicit PaddedFileManifestKey(std::string_view uri);

      std::span<std::byte const> bytes() const noexcept { return std::span{_bytes}.first(_size); }

    private:
      static constexpr std::size_t kCapacity = (LibraryUri::kMaxLength + 3U) & ~std::size_t{3U};

      std::array<std::byte, kCapacity> _bytes{};
      std::size_t _size = 0;
    };
  } // namespace detail

  struct ValidatedFileManifestEntry final
  {
    std::string_view uri;
  };

  Result<> validateFileManifestPayload(std::span<std::byte const> payload);
  Result<ValidatedFileManifestEntry> validateFileManifestEntry(std::span<std::byte const> rawKey,
                                                               std::span<std::byte const> payload);
} // namespace ao::library
