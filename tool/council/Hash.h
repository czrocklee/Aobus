// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/utility/Xxh3.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>

namespace ao::council
{
  // Incremental 64-bit XXH3 used by tree canaries.
  // Wraps the shared ao::utility accumulator and adds streamed-file mixing.
  class TreeHash64 final
  {
  public:
    void mix(std::string_view bytes) noexcept { _accumulator.mix(bytes); }

    // Streams a regular file through the hash in fixed-size chunks.
    Result<> mixFile(std::filesystem::path const& path)
    {
      constexpr std::size_t kChunkSize = 8192;

      auto input = std::ifstream{path, std::ios::binary};

      if (!input.is_open())
      {
        return makeError(Error::Code::IoError, "cannot read " + path.string());
      }

      auto buffer = std::array<char, kChunkSize>{};

      while (input)
      {
        input.read(buffer.data(), buffer.size());
        mix(std::string_view{buffer.data(), static_cast<std::size_t>(input.gcount())});
      }

      if (input.bad())
      {
        return makeError(Error::Code::IoError, "cannot read " + path.string());
      }

      return {};
    }

    std::uint64_t value() const noexcept { return _accumulator.value(); }

    // Canonical 16-hex-digit form used in fingerprints.
    std::string hex() const { return _accumulator.hex(); }

  private:
    utility::Xxh3Accumulator64 _accumulator;
  };
} // namespace ao::council
