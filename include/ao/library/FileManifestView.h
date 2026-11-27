// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/FileTimestamp.h>
#include <ao/library/FileManifestLayout.h>
#include <ao/library/FileStatus.h>
#include <ao/utility/Hash128.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace ao::library
{
  namespace detail
  {
    inline constexpr FileManifestHeader kZeroFileManifestHeader{};
  } // namespace detail

  /**
   * FileManifestView - Unified view of a file manifest entry.
   *
   * The O(1) structural gate requires an aligned header and the exact
   * record size for its segment count. On failure, isValid() is false and
   * field accessors return empty or zero values (Available for status).
   * FileManifestStore also checks record facts and treats post-open
   * validation failures as invariant faults.
   */
  class FileManifestView final
  {
  public:
    explicit FileManifestView(std::span<std::byte const> data) noexcept;

    /** True when the record passed its structural gate. */
    bool isValid() const noexcept { return _header != nullptr; }
    std::span<std::byte const> rawData() const noexcept { return _payload; }

    TrackId trackId() const noexcept;
    std::uint16_t segmentCount() const noexcept;
    std::span<TrackId const> segmentTrackIds() const noexcept;
    std::uint64_t totalFrames() const noexcept;
    std::uint64_t fileSize() const noexcept;
    std::optional<FileTimestamp> mtime() const noexcept;
    std::uint64_t audioPayloadLength() const noexcept;
    utility::Hash128 audioSignature() const noexcept;
    FileStatus status() const noexcept;

  private:
    FileManifestHeader const& header() const noexcept
    {
      return _header != nullptr ? *_header : detail::kZeroFileManifestHeader;
    }

    FileManifestHeader const* _header = nullptr;
    std::span<std::byte const> _payload;
  };
} // namespace ao::library
