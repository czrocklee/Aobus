// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/utility/MappedFile.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>

namespace ao::audio::detail
{
  enum class SeekOrigin : std::uint8_t
  {
    Begin,
    Current,
    End,
  };

  class MappedFileCursor final
  {
  public:
    Result<> open(std::filesystem::path const& filePath);
    void close() noexcept;

    std::size_t read(std::span<std::byte> destination) noexcept;
    Result<std::uint64_t> seek(std::int64_t offset, SeekOrigin origin) noexcept;

    bool isOpen() const noexcept;
    bool isAtEnd() const noexcept;
    std::uint64_t position() const noexcept;
    std::uint64_t size() const noexcept;

  private:
    utility::MappedFile _mappedFile;
    std::uint64_t _position = 0;
  };
} // namespace ao::audio::detail
