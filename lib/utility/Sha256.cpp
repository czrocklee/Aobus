// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/utility/Sha256.h>

#include "Hex.h"

#include <boost/hash2/sha2.hpp>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace ao::utility
{
  namespace
  {
    std::optional<std::byte> parseHexBytePair(char const high, char const low) noexcept
    {
      auto const highIndex = detail::kLowerHexDigits.find(high);
      auto const lowIndex = detail::kLowerHexDigits.find(low);

      if (highIndex == std::string_view::npos || lowIndex == std::string_view::npos)
      {
        return std::nullopt;
      }

      return static_cast<std::byte>((highIndex << detail::kNibbleBits) | lowIndex);
    }
  } // namespace

  Sha256Digest computeSha256(std::span<std::byte const> const data) noexcept
  {
    auto hash = boost::hash2::sha2_256{};

    if (!data.empty())
    {
      // An empty span may carry a null pointer, which update() would forward to
      // memcpy; the digest of no input is the same either way.
      hash.update(data.data(), data.size());
    }

    auto digest = Sha256Digest{};
    auto const result = hash.result();
    std::ranges::transform(
      result, digest.bytes.begin(), [](auto const value) { return static_cast<std::byte>(value); });

    return digest;
  }

  std::string sha256Hex(Sha256Digest const& digest)
  {
    return detail::lowerHex(digest.bytes);
  }

  std::optional<Sha256Digest> parseSha256Hex(std::string_view const text) noexcept
  {
    if (text.size() != Sha256Digest::kHexLength)
    {
      return std::nullopt;
    }

    auto digest = Sha256Digest{};

    for (std::size_t byteIndex = 0; byteIndex < Sha256Digest::kByteCount; ++byteIndex)
    {
      auto const optByte = parseHexBytePair(text[2 * byteIndex], text[(2 * byteIndex) + 1]);

      if (!optByte)
      {
        return std::nullopt;
      }

      digest.bytes[byteIndex] = *optByte;
    }

    return digest;
  }
} // namespace ao::utility
