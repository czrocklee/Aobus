// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "ao/Type.h"
#include "ao/lmdb/Database.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

namespace ao::library
{
  /**
   * FileStatus - Physical availability of a track.
   */
  enum class FileStatus : std::uint8_t
  {
    Available = 0,
    Missing = 1,
    Error = 2
  };

  /**
   * ManifestEntry - POD struct for physical file tracking.
   * Key: Normalized relative URI (String)
   * Total size: 24 bytes with 4-byte alignment.
   */
  struct ManifestEntry final
  {
    static constexpr std::size_t kPaddingSize = 3;

    TrackId trackId{};                             // 4B: Links to the logical track
    std::uint32_t fileSizeLo{};                    // 4B: Lower 32 bits of file size
    std::uint32_t fileSizeHi{};                    // 4B: Upper 32 bits of file size
    std::uint32_t mtimeLo{};                       // 4B: Lower 32 bits of mtime
    std::uint32_t mtimeHi{};                       // 4B: Upper 32 bits of mtime
    FileStatus status = FileStatus::Available;     // 1B
    std::array<std::byte, kPaddingSize> padding{}; // 3B: Alignment

    // Reconstruct 64-bit values
    std::uint64_t fileSize() const noexcept { return (static_cast<std::uint64_t>(fileSizeHi) << 32) | fileSizeLo; }

    std::uint64_t mtime() const noexcept { return (static_cast<std::uint64_t>(mtimeHi) << 32) | mtimeLo; }

    // Set 64-bit values
    void fileSize(std::uint64_t val) noexcept
    {
      fileSizeLo = static_cast<std::uint32_t>(val);
      fileSizeHi = static_cast<std::uint32_t>(val >> 32);
    }

    void mtime(std::uint64_t val) noexcept
    {
      mtimeLo = static_cast<std::uint32_t>(val);
      mtimeHi = static_cast<std::uint32_t>(val >> 32);
    }
  };

  static_assert(sizeof(ManifestEntry) == 24, "ManifestEntry must be exactly 24 bytes");

  /**
   * FileManifestStore - Manages the mapping between physical file paths and tracks.
   */
  class FileManifestStore final
  {
  public:
    class Reader;
    class Writer;

    explicit FileManifestStore(lmdb::Database db)
      : _db{std::move(db)}
    {
    }

    Reader reader(lmdb::ReadTransaction const& txn) const;
    Writer writer(lmdb::WriteTransaction& txn) const;

  private:
    lmdb::Database _db;
  };

  class FileManifestStore::Reader final
  {
  public:
    explicit Reader(lmdb::Database::Reader reader)
      : _reader{std::move(reader)}
    {
    }

    std::optional<ManifestEntry> get(std::string_view uri) const;

    lmdb::Database::Reader const& databaseReader() const noexcept { return _reader; }

    /**
     * Iterator for all manifest entries.
     */
    class Iterator;
    Iterator begin() const;
    Iterator end() const;

  private:
    lmdb::Database::Reader _reader;
  };

  class FileManifestStore::Writer final
  {
  public:
    explicit Writer(lmdb::Database::Writer writer)
      : _writer{std::move(writer)}
    {
    }

    void put(std::string_view uri, ManifestEntry const& entry);
    bool remove(std::string_view uri);
    void clear();

  private:
    lmdb::Database::Writer _writer;
  };
} // namespace ao::library
