// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/library/DictionaryStore.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/ListStore.h>
#include <ao/library/ResourceStore.h>
#include <ao/library/TrackStore.h>

namespace ao::library
{
  class WriteTransaction;
}

namespace ao::library::detail
{
  /**
   * Library-private access to the per-store writers a WriteTransaction owns.
   *
   * These live here rather than on WriteTransaction because their return types
   * are nested in the five store classes, and a member declaration would pull
   * all five store headers into every translation unit that only wants to hold
   * or apply a transaction. The writers are created on first use and owned by
   * the transaction, so each reference stays valid until the transaction ends.
   *
   * This is a production access path, distinct from the representation-test
   * seam in PhysicalStoreAccess.
   */
  class WriteTransactionAccess final
  {
  public:
    static DictionaryStore::Writer& dictionary(WriteTransaction& transaction);
    static TrackStore::Writer& trackStoreWriter(WriteTransaction& transaction);
    static ListStore::Writer& listStoreWriter(WriteTransaction& transaction);
    static FileManifestStore::Writer& manifestStoreWriter(WriteTransaction& transaction);
    static ResourceStore::Writer& resourceStoreWriter(WriteTransaction& transaction, ResourceStore const& resources);
  };
} // namespace ao::library::detail
