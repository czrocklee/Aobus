// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackStoreTestSupport.h"

#include "MusicLibraryTestSupport.h"
#include "WritableLibraryTestSupport.h"
#include "lib/library/FileManifestValidation.h"
#include "lib/library/TrackWrite.h"
#include "test/lmdb/LmdbTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/MetadataLayout.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackLayout.h>
#include <ao/library/TrackStore.h>
#include <ao/library/WriteTransaction.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>
#include <ao/utility/ByteView.h>

#include <catch2/catch_test_macros.hpp>
#include <lmdb.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

namespace ao::library::test
{
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
                       std::string_view const uri,
                       std::span<std::byte const> const hotData,
                       std::span<std::byte const> const coldData)
  {
    auto environment = lmdb::test::openEnvironment(path, {.flags = MDB_NOTLS, .maxDatabases = 8});
    auto transaction = lmdb::test::beginWriteTransaction(environment);
    auto hotDatabase = lmdb::test::openIntegerKeyDatabase(transaction, "tracks_hot");
    auto coldDatabase = lmdb::test::openIntegerKeyDatabase(transaction, "tracks_cold");

    if (!hotData.empty())
    {
      REQUIRE(hotDatabase.writer(transaction).create(rawTrackId, hotData));
      auto manifestDatabase = lmdb::test::openByteKeyDatabase(transaction, "file_manifest");
      auto const manifestKey = detail::PaddedFileManifestKey{uri};
      auto const manifestPayload = FileManifestBuilder::makeEmpty().trackId(TrackId{rawTrackId}).serialize();
      REQUIRE(manifestDatabase.writer(transaction).create(manifestKey.bytes(), manifestPayload));
    }

    if (!coldData.empty())
    {
      REQUIRE(coldDatabase.writer(transaction).create(rawTrackId, coldData));
    }

    coverRawIdSpace(transaction, rawTrackId);
    REQUIRE(transaction.commit());
  }

  void coverRawIdSpace(lmdb::WriteTransaction& transaction,
                       std::uint32_t const lastTrackId,
                       std::uint32_t const lastListId)
  {
    auto metadataDatabase = lmdb::test::openIntegerKeyDatabase(transaction, "meta");
    auto writer = metadataDatabase.writer(transaction);
    auto const optBytes = writer.get(kIdSpaceRecordId);
    REQUIRE(optBytes);
    REQUIRE(optBytes->size() == sizeof(IdSpace));
    auto idSpace = IdSpace{};
    std::memcpy(&idSpace, optBytes->data(), sizeof(idSpace));
    idSpace.lastTrackId = std::max(idSpace.lastTrackId, lastTrackId);
    idSpace.lastListId = std::max(idSpace.lastListId, lastListId);
    REQUIRE(writer.update(kIdSpaceRecordId, utility::bytes::view(idSpace)));
  }

  IdSpace readRawIdSpace(std::filesystem::path const& path)
  {
    auto const optBytes = lmdb::test::readExistingIntegerKeyRecord(path, "meta", kIdSpaceRecordId);
    REQUIRE(optBytes);
    REQUIRE(optBytes->size() == sizeof(IdSpace));
    auto idSpace = IdSpace{};
    std::memcpy(&idSpace, optBytes->data(), sizeof(idSpace));
    return idSpace;
  }

  void initializeLibraryStorage(std::filesystem::path const& path)
  {
    auto const libraryRes = openTestMusicLibrary(path, path);
    REQUIRE(libraryRes);
  }

  void requireCorruptOpen(std::filesystem::path const& path, std::string_view const expectedMessage)
  {
    auto const res = openTestMusicLibrary(path, path);
    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::CorruptData);
    CHECK(res.error().message == expectedMessage);
  }

  TrackId requireCreate(MusicLibrary& library, WriteTransaction& transaction, TrackBuilder const& builder)
  {
    auto preparedRes = physicalPrepareTrack(builder, transaction, library.resources());
    REQUIRE(preparedRes);

    auto writer = physicalWriter(library.tracks(), transaction);
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
