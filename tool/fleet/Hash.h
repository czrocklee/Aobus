// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>

namespace ao::fleet
{
  // Incremental 64-bit FNV-1a, shared by oracle version fingerprints and tree canaries.
  class Fnv1a64 final
  {
  public:
    void mix(std::string_view bytes)
    {
      for (auto const byte : bytes)
      {
        _hash ^= static_cast<unsigned char>(byte);
        _hash *= kPrime;
      }
    }

    // Streams a regular file through the hash in fixed-size chunks.
    void mixFile(std::filesystem::path const& path)
    {
      constexpr std::size_t kChunkSize = 8192;

      auto input = std::ifstream{path, std::ios::binary};
      auto buffer = std::array<char, kChunkSize>{};

      while (input)
      {
        input.read(buffer.data(), buffer.size());
        mix(std::string_view{buffer.data(), static_cast<std::size_t>(input.gcount())});
      }
    }

    std::uint64_t value() const noexcept { return _hash; }

    // Canonical 16-hex-digit form used in route keys and fingerprints.
    std::string hex() const { return std::format("{:016x}", _hash); }

  private:
    static constexpr auto kPrime = std::uint64_t{1099511628211ULL};
    static constexpr auto kOffsetBasis = std::uint64_t{1469598103934665603ULL};

    std::uint64_t _hash = kOffsetBasis;
  };
} // namespace ao::fleet
