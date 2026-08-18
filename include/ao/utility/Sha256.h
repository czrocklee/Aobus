// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

// SHA-256 for persisted content identity that has to resist deliberate
// construction, which the XXH3 wrappers explicitly do not. Boost.Hash2 is an
// implementation detail of Sha256.cpp; this header stays free of its types so
// library-layer headers never leak the dependency.
namespace ao::utility
{
  /// 256-bit content digest, in the byte order SHA-256 produces.
  struct Sha256Digest final
  {
    static constexpr std::size_t kByteCount = 32;
    static constexpr std::size_t kHexLength = 2 * kByteCount;

    std::array<std::byte, kByteCount> bytes{};

    constexpr bool operator==(Sha256Digest const&) const noexcept = default;
    constexpr auto operator<=>(Sha256Digest const&) const noexcept = default;
  };

  /// One-shot SHA-256 over @p data. An empty input yields the published
  /// digest of the empty string rather than a zeroed value.
  Sha256Digest computeSha256(std::span<std::byte const> data) noexcept;

  /// Fixed 64-character lower-case form. One spelling per digest, so a
  /// persisted or transferred digest compares as text without normalization.
  std::string sha256Hex(Sha256Digest const& digest);

  /// Accepts exactly the form sha256Hex() produces. An uppercase spelling, a
  /// short or long string, and any non-hexadecimal character are all refused,
  /// because a second spelling would make text comparison unsound.
  std::optional<Sha256Digest> parseSha256Hex(std::string_view text) noexcept;
} // namespace ao::utility
