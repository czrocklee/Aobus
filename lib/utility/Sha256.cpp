// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/utility/Sha256.h>

#include <boost/hash2/sha2.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace ao::utility
{
  namespace
  {
    constexpr auto kHexDigits = std::string_view{"0123456789abcdef"};
    constexpr auto kNibbleShift = 4U;
    constexpr auto kLowNibbleMask = 0x0FU;

    std::optional<std::byte> parseHexBytePair(char const high, char const low) noexcept
    {
      auto const highIndex = kHexDigits.find(high);
      auto const lowIndex = kHexDigits.find(low);

      if (highIndex == std::string_view::npos || lowIndex == std::string_view::npos)
      {
        return std::nullopt;
      }

      return static_cast<std::byte>((highIndex << kNibbleShift) | lowIndex);
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
    auto text = std::string{};
    text.reserve(Sha256Digest::kHexLength);

    for (auto const value : digest.bytes)
    {
      auto const raw = std::to_integer<std::uint32_t>(value);
      text.push_back(kHexDigits[raw >> kNibbleShift]);
      text.push_back(kHexDigits[raw & kLowNibbleMask]);
    }

    return text;
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
