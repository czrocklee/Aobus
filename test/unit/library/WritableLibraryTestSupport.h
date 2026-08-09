// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "lib/library/PhysicalStoreAccess.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/WritableMusicLibrary.h>
#include <ao/library/WriteTransaction.h>

#include <cstddef>
#include <span>

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::library::test
{
  WritableMusicLibrary requireWritableLibrary(MusicLibrary& library);
  WriteTransaction writeTransaction(MusicLibrary& library, WriteTransaction::Options options = {});

  template<typename Store>
  auto physicalWriter(Store const& store, WriteTransaction& transaction)
  {
    return detail::PhysicalStoreAccess::writer(store, transaction);
  }

  template<typename Store>
  auto physicalWriter(Store const& store, LibraryWrite& write)
  {
    return detail::PhysicalStoreAccess::writer(store, write);
  }

  inline DictionaryStore::Writer& physicalDictionary(WriteTransaction& transaction)
  {
    return detail::PhysicalStoreAccess::dictionary(transaction);
  }

  inline Result<> overwriteListRecordForTest(ListStore const& store,
                                             LibraryWrite& write,
                                             ListId const id,
                                             std::span<std::byte const> const bytes)
  {
    return detail::PhysicalStoreAccess::overwriteListRecordForTest(store, write, id, bytes);
  }

  inline auto physicalPrepareTrack(TrackBuilder const& builder,
                                   WriteTransaction& transaction,
                                   ResourceStore const& resources)
  {
    return detail::PhysicalStoreAccess::prepareTrack(builder, transaction, resources);
  }

  inline auto physicalPrepareHotTrack(TrackBuilder const& builder, WriteTransaction& transaction)
  {
    return detail::PhysicalStoreAccess::prepareHotTrack(builder, transaction);
  }

  inline auto physicalPrepareColdTrack(TrackBuilder const& builder,
                                       WriteTransaction& transaction,
                                       ResourceStore const& resources)
  {
    return detail::PhysicalStoreAccess::prepareColdTrack(builder, transaction, resources);
  }

  inline auto physicalSerializeTrack(TrackBuilder const& builder,
                                     WriteTransaction& transaction,
                                     ResourceStore const& resources)
  {
    return detail::PhysicalStoreAccess::serializeTrack(builder, transaction, resources);
  }

  inline auto physicalSerializeHotTrack(TrackBuilder const& builder, WriteTransaction& transaction)
  {
    return detail::PhysicalStoreAccess::serializeHotTrack(builder, transaction);
  }

  inline auto physicalSerializeColdTrack(TrackBuilder const& builder,
                                         WriteTransaction& transaction,
                                         ResourceStore const& resources)
  {
    return detail::PhysicalStoreAccess::serializeColdTrack(builder, transaction, resources);
  }
} // namespace ao::library::test
