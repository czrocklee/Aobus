// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackStoreTestSupport.h"

#include "MusicLibraryTestSupport.h"
#include "WritableLibraryTestSupport.h"
#include "test/unit/lmdb/LmdbTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackLayout.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackWrite.h>
#include <ao/library/WriteTransaction.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>

#include <catch2/catch_test_macros.hpp>
#include <lmdb.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

namespace ao::library::test
{
  namespace
  {
    constexpr std::string_view kMissingMetadataMessage = "Library data exists without a metadata header";
  }

  TrackStoreFixture::TrackStoreFixture()
    : temp{}, library{makeTestMusicLibrary(temp.path(), temp.path() / "db")}, store{library.tracks()}
  {
  }

  std::vector<std::byte> makeHotData(TrackHotHeader header, std::string_view title)
  {
    header.titleLength = static_cast<std::uint16_t>(title.size());

    auto data = std::vector<std::byte>(alignToWord(sizeof(TrackHotHeader) + title.size()), std::byte{0});
    std::memcpy(data.data(), &header, sizeof(TrackHotHeader));

    if (!title.empty())
    {
      std::memcpy(data.data() + sizeof(TrackHotHeader), title.data(), title.size());
    }

    return data;
  }

  std::vector<std::byte> makeColdData(TrackColdHeader header, std::string_view const uri)
  {
    header.blockOffsets = {};
    header.uriOffset = sizeof(TrackColdHeader);
    header.uriLength = static_cast<std::uint16_t>(uri.size());

    auto data = std::vector<std::byte>(alignToWord(sizeof(TrackColdHeader) + uri.size()), std::byte{0});
    std::memcpy(data.data(), &header, sizeof(TrackColdHeader));
    std::memcpy(data.data() + sizeof(TrackColdHeader), uri.data(), uri.size());
    return data;
  }

  void seedRawTrackRow(std::filesystem::path const& path,
                       std::uint32_t const rawTrackId,
                       std::span<std::byte const> const hotData,
                       std::span<std::byte const> const coldData)
  {
    auto environment = lmdb::test::openEnvironment(path, {.flags = MDB_NOTLS, .maxDatabases = 8});
    auto transaction = lmdb::test::beginWriteTransaction(environment);
    auto hotDatabase = lmdb::test::openDatabase(transaction, "tracks_hot");
    auto coldDatabase = lmdb::test::openDatabase(transaction, "tracks_cold");

    if (!hotData.empty())
    {
      REQUIRE(hotDatabase.writer(transaction).create(rawTrackId, hotData));
    }

    if (!coldData.empty())
    {
      REQUIRE(coldDatabase.writer(transaction).create(rawTrackId, coldData));
    }

    REQUIRE(transaction.commit());
  }

  void initializeLibraryStorage(std::filesystem::path const& path)
  {
    auto const libraryRes = openTestMusicLibrary(path, path);
    REQUIRE(libraryRes);
  }

  void requireCorruptOpen(std::filesystem::path const& path)
  {
    auto const result = openTestMusicLibrary(path, path);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::CorruptData);

    // Seeded rows without a metadata header fail before any record sweep runs,
    // so a record-integrity case that forgets initializeLibraryStorage would
    // otherwise pass without ever reaching the validator it means to exercise.
    CHECK(result.error().message != kMissingMetadataMessage);
  }

  TrackId requireCreate(MusicLibrary& library, WriteTransaction& transaction, TrackBuilder const& builder)
  {
    auto preparedRes = builder.prepare(transaction, library.resources());
    REQUIRE(preparedRes);

    auto writer = library.tracks().writer(transaction);
    auto createdRes = createPreparedTrackRecord(writer, preparedRes->first, preparedRes->second);
    REQUIRE(createdRes);
    return *createdRes;
  }

  TrackId createCommittedTrack(MusicLibrary& library, TrackBuilder const& builder)
  {
    auto transaction = writeTransaction(library);
    auto const created = requireCreate(library, transaction, builder);
    REQUIRE(transaction.commit());
    return created;
  }
} // namespace ao::library::test
