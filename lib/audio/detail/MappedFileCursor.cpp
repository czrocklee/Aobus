// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "MappedFileCursor.h"

#include <ao/Error.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <limits>
#include <span>

namespace ao::audio::detail
{
  Result<> MappedFileCursor::open(std::filesystem::path const& filePath)
  {
    close();

    if (auto const result = _mappedFile.map(filePath); !result)
    {
      return std::unexpected{result.error()};
    }

    return {};
  }

  void MappedFileCursor::close() noexcept
  {
    _mappedFile.unmap();
    _position = 0;
  }

  std::size_t MappedFileCursor::read(std::span<std::byte> destination) noexcept
  {
    auto const bytes = _mappedFile.bytes();

    if (destination.empty() || _position >= bytes.size())
    {
      return 0;
    }

    auto const remaining = bytes.size() - static_cast<std::size_t>(_position);
    auto const count = std::min(destination.size(), remaining);
    std::memcpy(destination.data(), bytes.data() + _position, count);
    _position += count;
    return count;
  }

  Result<std::uint64_t> MappedFileCursor::seek(std::int64_t offset, SeekOrigin origin) noexcept
  {
    auto const fileSize = size();
    auto base = std::uint64_t{0};

    switch (origin)
    {
      case SeekOrigin::Begin: base = 0; break;
      case SeekOrigin::Current: base = _position; break;
      case SeekOrigin::End: base = fileSize; break;
    }

    std::uint64_t target = 0;

    if (offset < 0)
    {
      auto const magnitude = static_cast<std::uint64_t>(-(offset + 1)) + 1U;

      if (magnitude > base)
      {
        return makeError(Error::Code::SeekFailed, "Mapped file seek is out of bounds");
      }

      target = base - magnitude;
    }
    else
    {
      auto const positiveOffset = static_cast<std::uint64_t>(offset);

      if (positiveOffset > std::numeric_limits<std::uint64_t>::max() - base)
      {
        return makeError(Error::Code::SeekFailed, "Mapped file seek overflowed");
      }

      target = base + positiveOffset;
    }

    if (target > fileSize)
    {
      return makeError(Error::Code::SeekFailed, "Mapped file seek is out of bounds");
    }

    _position = target;
    return target;
  }

  bool MappedFileCursor::isOpen() const noexcept
  {
    return _mappedFile.isMapped();
  }

  bool MappedFileCursor::atEnd() const noexcept
  {
    return !isOpen() || _position >= size();
  }

  std::uint64_t MappedFileCursor::position() const noexcept
  {
    return _position;
  }

  std::uint64_t MappedFileCursor::size() const noexcept
  {
    return _mappedFile.bytes().size();
  }
} // namespace ao::audio::detail
