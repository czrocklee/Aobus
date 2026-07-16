// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/lmdb/Transaction.h>

namespace ao::library
{
  namespace detail
  {
    class LibraryIdentity;
  }

  class FileManifestStore;
  class ListStore;
  class MetadataStore;
  class MusicLibrary;
  class ResourceStore;
  class TrackStore;

  /**
   * Owns one coherent, read-only library snapshot.
   *
   * The native LMDB transaction is intentionally hidden so library consumers
   * cannot use this capability with an unrelated storage surface.
   */
  class [[nodiscard]] ReadTransaction final
  {
  public:
    ~ReadTransaction() = default;

    ReadTransaction(ReadTransaction const&) = delete;
    ReadTransaction& operator=(ReadTransaction const&) = delete;
    ReadTransaction(ReadTransaction&&) noexcept = default;
    ReadTransaction& operator=(ReadTransaction&&) noexcept = default;

  private:
    ReadTransaction(lmdb::ReadTransaction transaction, detail::LibraryIdentity const& identity) noexcept;

    lmdb::ReadTransaction const& native(detail::LibraryIdentity const& identity) const;

    lmdb::ReadTransaction _transaction;
    detail::LibraryIdentity const* _identity;

    friend class FileManifestStore;
    friend class ListStore;
    friend class MetadataStore;
    friend class MusicLibrary;
    friend class ResourceStore;
    friend class TrackStore;
  };
} // namespace ao::library
