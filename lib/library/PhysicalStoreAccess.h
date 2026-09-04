// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "WriteTransactionAccess.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/ListStore.h>
#include <ao/library/ResourceStore.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/library/WriteTransaction.h>

#include <cstddef>
#include <span>

namespace ao::library::detail
{
  /** Source-private representation-test seam; production code must use logical mutation ports. */
  class PhysicalStoreAccess final
  {
  public:
    static WriteTransaction& transaction(LibraryWrite& write) { return write.transaction(); }

    static auto prepareTrack(TrackBuilder const& builder, WriteTransaction& transaction, ResourceStore const& resources)
    {
      return builder.prepare(transaction, resources);
    }

    static auto prepareHotTrack(TrackBuilder const& builder, WriteTransaction& transaction)
    {
      return builder.prepareHot(transaction);
    }

    static auto prepareColdTrack(TrackBuilder const& builder,
                                 WriteTransaction& transaction,
                                 ResourceStore const& resources)
    {
      return builder.prepareCold(transaction, resources);
    }

    static auto serializeTrack(TrackBuilder const& builder,
                               WriteTransaction& transaction,
                               ResourceStore const& resources)
    {
      return builder.serialize(transaction, resources);
    }

    static auto serializeHotTrack(TrackBuilder const& builder, WriteTransaction& transaction)
    {
      return builder.serializeHot(transaction);
    }

    static auto serializeColdTrack(TrackBuilder const& builder,
                                   WriteTransaction& transaction,
                                   ResourceStore const& resources)
    {
      return builder.serializeCold(transaction, resources);
    }

    static TrackStore::Writer writer(TrackStore const& store, WriteTransaction& transaction)
    {
      return store.writer(transaction);
    }

    static TrackStore::Writer writer(TrackStore const& store, LibraryWrite& write)
    {
      return writer(store, transaction(write));
    }

    static bool removeHotTrackRecordForTest(TrackStore const& store, WriteTransaction& transaction, TrackId const id)
    {
      return store._hotDb.writer(transaction.native(*store._identity)).del(id.raw());
    }

    static bool removeColdTrackRecordForTest(TrackStore const& store, WriteTransaction& transaction, TrackId const id)
    {
      return store._coldDb.writer(transaction.native(*store._identity)).del(id.raw());
    }

    static ListStore::Writer writer(ListStore const& store, WriteTransaction& transaction)
    {
      return store.writer(transaction);
    }

    static ListStore::Writer writer(ListStore const& store, LibraryWrite& write)
    {
      return writer(store, transaction(write));
    }

    static FileManifestStore::Writer writer(FileManifestStore const& store, WriteTransaction& transaction)
    {
      return store.writer(transaction);
    }

    static FileManifestStore::Writer writer(FileManifestStore const& store, LibraryWrite& write)
    {
      return writer(store, transaction(write));
    }

    static ResourceStore::Writer writer(ResourceStore const& store, WriteTransaction& transaction)
    {
      return store.writer(transaction);
    }

    static ResourceStore::Writer writer(ResourceStore const& store, LibraryWrite& write)
    {
      return writer(store, transaction(write));
    }

    static DictionaryStore::Writer& dictionary(WriteTransaction& transaction)
    {
      return WriteTransactionAccess::dictionary(transaction);
    }

    static DictionaryStore::Writer& dictionary(LibraryWrite& write)
    {
      return WriteTransactionAccess::dictionary(write.transaction());
    }

    static Result<> overwriteListRecordForTest(ListStore const& store,
                                               LibraryWrite& write,
                                               ListId const id,
                                               std::span<std::byte const> const bytes)
    {
      return store._database.writer(write.transaction().native(*store._identity)).update(id.raw(), bytes);
    }
  };
} // namespace ao::library::detail
