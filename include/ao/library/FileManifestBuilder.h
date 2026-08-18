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
    class Unbound;

    static FileManifestBuilder makeEmpty();
    static FileManifestBuilder fromView(FileManifestView const& view);

    // Only fromView() round-trips and raw serialize() diagnostics observe this
    // id. validate() discards it, and a Store write always takes its binding
    // from Unbound::bind().
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

      friend class Unbound;
    };

    /**
     * A canonical key and validated record facts that still lack their Track
     * binding. Its Track id is always zero, so a caller-supplied or stale
     * builder id can never reach storage. Only bind() produces a storable
     * value, which makes a completed validation and a real Track id the single
     * path into the Store.
     */
    class Unbound final
    {
    public:
      std::string_view uri() const noexcept { return _uri.value(); }

      /**
       * Binds the owning Track and yields the storable value. The id must be
       * nonzero. Binding consumes this value and cannot fail; a consumed value
       * has surrendered its canonical key, so binding it again is a caller
       * call-order violation rather than a recoverable error.
       */
      Prepared bind(TrackId id) && noexcept;

    private:
      Unbound(LibraryUri uri, FileManifestHeader header);

      LibraryUri _uri;
      FileManifestHeader _header;

      friend class FileManifestBuilder;
    };

    /**
     * Parses the URI and applies the canonical key validator plus every record
     * fact that does not depend on the owning Track binding, without allocating
     * a serialized payload.
     */
    Result<Unbound> validate(std::string_view uri) const;
    Result<Unbound> validate(LibraryUri uri) const;

    // Raw serialization remains available for binary-layout and corruption
    // diagnostics; production Store writers accept Prepared only.
    std::vector<std::byte> serialize() const;

  private:
    FileManifestHeader _header;
  };
} // namespace ao::library
