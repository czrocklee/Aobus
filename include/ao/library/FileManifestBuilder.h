// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/FileManifestLayout.h>
#include <ao/library/LibraryUri.h>
#include <ao/utility/Hash128.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace ao::library
{
  class FileManifestView;

  /**
   * FileManifestBuilder - Fluent builder for constructing file manifest binary data.
   */
  class FileManifestBuilder final
  {
  public:
    static FileManifestBuilder makeEmpty();
    static FileManifestBuilder fromView(FileManifestView const& view);

    FileManifestBuilder& trackId(TrackId val);
    FileManifestBuilder& fileSize(std::uint64_t val);
    FileManifestBuilder& mtime(std::uint64_t val);
    FileManifestBuilder& audioPayloadLength(std::uint64_t val);
    FileManifestBuilder& audioSignature(utility::Hash128 val);
    FileManifestBuilder& status(FileStatus val);

    /** Immutable, canonically validated key and value for one Store write. */
    class Prepared final
    {
    public:
      std::string_view uri() const noexcept { return _uri.value(); }
      std::span<std::byte const> bytes() const noexcept { return std::as_bytes(std::span{&_header, std::size_t{1}}); }

    private:
      Prepared(LibraryUri uri, FileManifestHeader header);

      LibraryUri _uri;
      FileManifestHeader _header;

      friend class FileManifestBuilder;
    };

    Result<Prepared> prepare(std::string_view uri) const;
    Result<Prepared> prepare(LibraryUri uri) const;

    // Raw serialization remains available for binary-layout and corruption
    // diagnostics; production Store writers accept Prepared only.
    std::vector<std::byte> serialize() const;

  private:
    FileManifestHeader _header;
  };
} // namespace ao::library
