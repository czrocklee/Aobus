// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/utility/Fnv1a.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>

namespace ao::fleet
{
  // Incremental 64-bit FNV-1a, shared by oracle version fingerprints and tree canaries.
  // Wraps the shared ao::utility accumulator and adds streamed-file mixing.
  class Fnv1a64 final
  {
  public:
    void mix(std::string_view bytes) noexcept { _accumulator.mix(bytes); }

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

    std::uint64_t value() const noexcept { return _accumulator.value(); }

    // Canonical 16-hex-digit form used in route keys and fingerprints.
    std::string hex() const { return _accumulator.hex(); }

  private:
    utility::Fnv1a64Accumulator _accumulator;
  };
} // namespace ao::fleet
