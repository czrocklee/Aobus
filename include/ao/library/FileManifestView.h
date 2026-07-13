// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/library/FileManifestLayout.h>
#include <ao/utility/Hash128.h>

#include <cstddef>
#include <cstdint>
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
   * Record contract (see doc/reference/library/storage/database.md):
   * the constructor runs the O(1) structural gate (fixed-size header fits
   * and is aligned). A record that fails the gate is a poisoned view:
   * isValid() reports false and accessors return zero values. Accessors
   * never throw. FileManifestStore additionally reports a short record as
   * CorruptData at its Reader/Writer::get boundary.
   */
  class FileManifestView final
  {
  public:
    explicit FileManifestView(std::span<std::byte const> data) noexcept;

    /** True when the record passed its structural gate. */
    bool isValid() const noexcept { return _header != nullptr; }

    TrackId trackId() const noexcept;
    std::uint64_t fileSize() const noexcept;
    std::uint64_t mtime() const noexcept;
    std::uint64_t audioPayloadLength() const noexcept;
    utility::Hash128 audioSignature() const noexcept;
    FileStatus status() const noexcept;

  private:
    FileManifestHeader const& header() const noexcept
    {
      return _header != nullptr ? *_header : detail::kZeroFileManifestHeader;
    }

    FileManifestHeader const* _header = nullptr;
  };
} // namespace ao::library
