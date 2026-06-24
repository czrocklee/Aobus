// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/utility/Base64.h>

#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ao::utility
{
  namespace
  {
    constexpr auto kAlphabet = std::string_view{"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"};

    constexpr auto kBase64BitsPerChar = 6;
    constexpr auto kBitsPerByte = 8;
    constexpr auto kBase64ChunkSize = 3;
    constexpr auto kBase64EncodedChunkSize = 4;
    constexpr auto kBase64Mask = 0x3F;
    constexpr auto kByteMask = 0xFF;

    constexpr std::array<std::int8_t, 256> kDecodingTable = []
    {
      auto table = std::array<std::int8_t, 256>{};
      table.fill(-1);

      for (std::size_t i = 0; i < kAlphabet.size(); ++i)
      {
        table.at(static_cast<std::uint8_t>(kAlphabet[i])) = static_cast<std::int8_t>(i);
      }

      return table;
    }();
  } // namespace

  std::string base64Encode(std::span<std::byte const> data)
  {
    auto result = std::string{};
    result.reserve(((data.size() + 2) / kBase64ChunkSize) * kBase64EncodedChunkSize);

    std::uint32_t buffer = 0;
    std::int32_t bits = 0;

    for (auto const byteVal : data)
    {
      buffer = (buffer << kBitsPerByte) | static_cast<std::uint8_t>(byteVal);
      bits += kBitsPerByte;

      while (bits >= kBase64BitsPerChar)
      {
        bits -= kBase64BitsPerChar;
        result.push_back(kAlphabet[(buffer >> bits) & kBase64Mask]);
        buffer &= (1U << bits) - 1U;
      }
    }

    if (bits > 0)
    {
      buffer <<= (kBase64BitsPerChar - bits);
      result.push_back(kAlphabet[buffer & kBase64Mask]);
    }

    while (result.size() % kBase64EncodedChunkSize != 0)
    {
      result.push_back('=');
    }

    return result;
  }

  std::optional<std::vector<std::byte>> base64Decode(std::string_view base64)
  {
    auto result = std::vector<std::byte>{};
    result.reserve((base64.size() / kBase64EncodedChunkSize) * kBase64ChunkSize);

    std::uint32_t buffer = 0;
    std::int32_t bits = 0;

    for (auto const charVal : base64)
    {
      if (std::isspace(static_cast<unsigned char>(charVal)) != 0)
      {
        continue;
      }

      if (charVal == '=')
      {
        break;
      }

      // The index is a uint8_t (0-255) and the table has 256 entries, so the
      // lookup is always in bounds; operator[] avoids a redundant bounds check.
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
      auto const decodedValue = kDecodingTable[static_cast<std::uint8_t>(charVal)];

      if (decodedValue == -1)
      {
        return std::nullopt; // Invalid character
      }

      buffer = (buffer << kBase64BitsPerChar) | static_cast<std::uint32_t>(decodedValue);
      bits += kBase64BitsPerChar;

      if (bits >= kBitsPerByte)
      {
        bits -= kBitsPerByte;
        result.push_back(static_cast<std::byte>((buffer >> bits) & kByteMask));
        buffer &= (1U << bits) - 1U;
      }
    }

    if (bits >= kBase64BitsPerChar || (bits > 0 && buffer != 0))
    {
      return std::nullopt;
    }

    return result;
  }
} // namespace ao::utility
