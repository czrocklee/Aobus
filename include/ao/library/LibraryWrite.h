// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/library/ListWriter.h>
#include <ao/library/TrackWriter.h>

#include <array>
#include <cstddef>

namespace ao::lmdb
{
  class WriteTransaction;
}

namespace ao::library
{
  namespace detail
  {
    class LibraryIdentity;
    class PhysicalStoreAccess;
  }

  class FileManifestStore;
  class ListStore;
  class MusicLibrary;
  class ResourceStore;
  class TrackStore;
  class WriteTransaction;

  /**
   * Callback-scoped logical mutation capability for one WriteTransaction root.
   *
   * WriteTransaction::apply() is the only constructor. The capability exposes
   * logical aggregate mutation but not commit or abort, and every operation
   * requires the owning apply() callback to remain active.
   */
  class LibraryWrite final
  {
  public:
    ~LibraryWrite() = default;

    LibraryWrite(LibraryWrite const&) = delete;
    LibraryWrite& operator=(LibraryWrite const&) = delete;
    LibraryWrite(LibraryWrite&&) = delete;
    LibraryWrite& operator=(LibraryWrite&&) = delete;

    TrackWriter tracks();
    ListWriter lists();
    Result<> restoreLibraryIdentity(std::array<std::byte, 16> const& libraryId);

  private:
    explicit LibraryWrite(WriteTransaction& transaction) noexcept;
    WriteTransaction& transaction();

    lmdb::WriteTransaction const& native(detail::LibraryIdentity const& identity) const;

    WriteTransaction* _transaction;

    friend class FileManifestStore;
    friend class ListStore;
    friend class MusicLibrary;
    friend class ResourceStore;
    friend class TrackStore;
    friend class WriteTransaction;
    friend class detail::PhysicalStoreAccess;
  };
} // namespace ao::library
