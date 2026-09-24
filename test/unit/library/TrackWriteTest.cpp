// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "lib/library/TrackWrite.h"

#include "lib/library/TrackRecordValidation.h"
#include "lib/lmdb/detail/TransactionFailure.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/TrackStoreTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/ResourceStore.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackWriter.h>
#include <ao/library/WriteTransaction.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ao::library::test
{
  namespace
  {
    // Records reach storage only as prepared values, serialized straight into
    // storage-owned bytes. Reintroducing an entry point that takes caller-supplied
    // record bytes would bring back a second write path with its own copy and its
    // own validation, so the writer must keep rejecting one.
    template<typename Writer>
    concept HasRawRecordCreate =
      requires(Writer& writer) { writer.createHotCold(std::span<std::byte const>{}, std::span<std::byte const>{}); };

    template<typename Writer>
    concept HasRawRecordUpdate =
      requires(Writer& writer) { writer.updateHot(TrackId{1}, std::span<std::byte const>{}); };

    static_assert(!HasRawRecordCreate<TrackStore::Writer>);
    static_assert(!HasRawRecordUpdate<TrackStore::Writer>);
    static_assert(noexcept(std::declval<TrackBuilder::PreparedHot const&>().writeTo(std::span<std::byte>{})));
    static_assert(noexcept(std::declval<TrackBuilder::PreparedCold const&>().writeTo(std::span<std::byte>{})));

    std::pair<TrackBuilder::PreparedHot, TrackBuilder::PreparedCold> prepareTrack(TrackBuilder& builder,
                                                                                  WriteTransaction& transaction,
                                                                                  ResourceStore const& resources)
    {
      auto res = physicalPrepareTrack(builder, transaction, resources);
      REQUIRE(res);
      return *std::move(res);
    }
  } // namespace

  TEST_CASE("createPreparedTrackRecord writes prepared hot and cold track records", "[library][unit][track]")
  {
    auto fixture = TrackStoreFixture{};
    auto transaction = writeTransaction(fixture.library);
    auto builder = TrackBuilder::makeEmpty();
    builder.metadata().title("Created Track").artist("Artist");
    builder.property().uri("created.flac");

    auto const [preparedHot, preparedCold] = prepareTrack(builder, transaction, fixture.library.resources());
    auto writer = physicalWriter(fixture.store, transaction);

    auto createRes = createPreparedTrackRecord(writer, preparedHot, preparedCold);
    REQUIRE(createRes);

    auto const trackId = *createRes;
    auto const optView = writer.get(trackId, TrackStore::Reader::LoadMode::Both);
    REQUIRE(optView);
    CHECK(trackId != kInvalidTrackId);
    CHECK(optView->metadata().title() == "Created Track");
    CHECK(optView->property().uri() == "created.flac");
    REQUIRE(transaction.commit());

    auto readTransaction = fixture.library.readTransaction();
    auto const optCommitted = fixture.store.reader(readTransaction).get(trackId, TrackStore::Reader::LoadMode::Both);
    REQUIRE(optCommitted);
    CHECK(optCommitted->metadata().title() == "Created Track");
    CHECK(optCommitted->property().uri() == "created.flac");
  }

  TEST_CASE("TrackStore - prepared records emit canonical hot and cold bytes", "[library][unit][track-store]")
  {
    auto fixture = TrackStoreFixture{};
    auto transaction = writeTransaction(fixture.library);
    auto builder = TrackBuilder::makeEmpty();
    builder.metadata().title("Odd title").artist("Artist");
    builder.tags().add("favorite");
    builder.customMetadata().add("mood", "calm");
    builder.property().uri("odd.flac");
    auto const [preparedHot, preparedCold] = prepareTrack(builder, transaction, fixture.library.resources());
    auto hotBytes = std::vector<std::byte>(preparedHot.size());
    auto coldBytes = std::vector<std::byte>(preparedCold.size());

    preparedHot.writeTo(hotBytes);
    preparedCold.writeTo(coldBytes);

    REQUIRE(validateSerializedHotTrack(hotBytes));
    REQUIRE(validateSerializedColdTrack(coldBytes));
  }

  TEST_CASE("updatePreparedTrackRecord replaces existing hot and cold track records", "[library][unit][track]")
  {
    auto fixture = TrackStoreFixture{};
    auto transaction = writeTransaction(fixture.library);
    auto originalBuilder = TrackBuilder::makeEmpty();
    originalBuilder.metadata().title("Original Track");
    originalBuilder.property().uri("original.flac");
    auto const [originalHot, originalCold] = prepareTrack(originalBuilder, transaction, fixture.library.resources());

    auto updatedBuilder = TrackBuilder::makeEmpty();
    updatedBuilder.metadata().title("Updated Track");
    updatedBuilder.property().uri("updated.flac");
    auto const [updatedHot, updatedCold] = prepareTrack(updatedBuilder, transaction, fixture.library.resources());

    auto writer = physicalWriter(fixture.store, transaction);
    auto createRes = createPreparedTrackRecord(writer, originalHot, originalCold);
    REQUIRE(createRes);

    auto const trackId = *createRes;
    auto updateRes = updatePreparedTrackRecord(writer, trackId, updatedHot, updatedCold);
    REQUIRE(updateRes);

    auto optView = writer.get(trackId, TrackStore::Reader::LoadMode::Both);
    REQUIRE(optView);
    CHECK(optView->metadata().title() == "Updated Track");
    CHECK(optView->property().uri() == "updated.flac");
    REQUIRE(transaction.commit());

    auto readTransaction = fixture.library.readTransaction();
    auto const optCommitted = fixture.store.reader(readTransaction).get(trackId, TrackStore::Reader::LoadMode::Both);
    REQUIRE(optCommitted);
    CHECK(optCommitted->metadata().title() == "Updated Track");
    CHECK(optCommitted->property().uri() == "updated.flac");
  }

  TEST_CASE("prepared track data is a snapshot unaffected by later builder mutation", "[library][unit][track]")
  {
    auto fixture = TrackStoreFixture{};
    auto transaction = writeTransaction(fixture.library);
    auto builder = TrackBuilder::makeEmpty();

    auto const [preparedHot, preparedCold] = [&]
    {
      // Borrowed input strings die before the prepared values are consumed.
      auto const title = std::string{"Snapshot Title"};
      auto const uri = std::string{"snapshot.flac"};
      builder.metadata().title(title).trackNumber(3);
      builder.property().uri(uri);
      return prepareTrack(builder, transaction, fixture.library.resources());
    }();

    auto const longerTitle = std::string{"Mutated Title That Is Much Longer Than Before"};
    auto const longerUri = std::string{"mutated/path/that/is/much/longer.flac"};
    builder.metadata().title(longerTitle).trackNumber(9);
    builder.property().uri(longerUri);

    auto writer = physicalWriter(fixture.store, transaction);
    auto createRes = createPreparedTrackRecord(writer, preparedHot, preparedCold);
    REQUIRE(createRes);

    auto const trackId = *createRes;
    auto const optView = writer.get(trackId, TrackStore::Reader::LoadMode::Both);
    REQUIRE(optView);
    CHECK(trackId != kInvalidTrackId);
    CHECK(optView->isHotValid());
    CHECK(optView->isColdValid());
    CHECK(optView->metadata().title() == "Snapshot Title");
    CHECK(optView->metadata().trackNumber() == 3);
    CHECK(optView->property().uri() == "snapshot.flac");
    REQUIRE(transaction.commit());
  }

  TEST_CASE("updatePreparedTrackRecord rolls back its hot update when the cold reservation fails",
            "[library][unit][track-store]")
  {
    constexpr std::uint64_t kInitialMapSize = std::uint64_t{1} * 1024 * 1024;
    constexpr std::uint64_t kUpdateHeadroom = std::uint64_t{32} * 1024;
    constexpr std::size_t kOversizedValue = std::size_t{60} * 1024;
    auto const temp = ao::test::TempDir{};
    auto trackId = kInvalidTrackId;
    auto optPrepared = std::optional<std::pair<TrackBuilder::PreparedHot, TrackBuilder::PreparedCold>>{};
    std::uint64_t tightMapSize = 0;

    {
      auto library = ao::test::requireValue(
        MusicLibrary::open(temp.path(), temp.path(), MusicLibrary::Options{.pinnedMapBytes = kInitialMapSize}));
      auto originalTransaction = writeTransaction(library);
      auto originalBuilder = TrackBuilder::makeEmpty();
      originalBuilder.metadata().title("Original");
      originalBuilder.property().uri("original.flac");
      auto const createRes =
        originalTransaction.apply([&originalBuilder](LibraryWrite& write)
                                  { return write.tracks().create(originalBuilder, FileManifestBuilder::makeEmpty()); });
      REQUIRE(createRes);
      trackId = *createRes;

      auto updatedBuilder = TrackBuilder::makeEmpty();
      updatedBuilder.metadata().title("Must roll back");
      // This canonical cold record needs more than the deliberately retained
      // headroom, while the preceding hot replacement fits in one page.
      auto const oversizedValue = std::string(kOversizedValue, 'x');
      updatedBuilder.property().uri("oversized.flac");
      updatedBuilder.customMetadata().add("oversized", oversizedValue);
      optPrepared.emplace(prepareTrack(updatedBuilder, originalTransaction, library.resources()));
      REQUIRE(originalTransaction.commit());

      // highWaterBytes is page-aligned on each host. The byte headroom becomes
      // eight 4 KiB pages on Intel and two 16 KiB pages on Apple Silicon.
      tightMapSize = library.storageCapacity().highWaterBytes + kUpdateHeadroom;
    }

    REQUIRE(optPrepared);
    auto library = ao::test::requireValue(
      MusicLibrary::open(temp.path(), temp.path(), MusicLibrary::Options{.pinnedMapBytes = tightMapSize}));

    auto optFailure = std::optional<Error>{};

    {
      auto updateTransaction = writeTransaction(library);
      auto const& [updatedHot, updatedCold] = *optPrepared;
      auto updateWriter = physicalWriter(library.tracks(), updateTransaction);

      try
      {
        [[maybe_unused]] auto res = updatePreparedTrackRecord(updateWriter, trackId, updatedHot, updatedCold);
        FAIL("updatePreparedTrackRecord should abort when the cold reservation cannot fit");
      }
      catch (lmdb::detail::TransactionFailure const& transactionFailure)
      {
        optFailure = transactionFailure.error();
      }
    }

    REQUIRE(optFailure);
    CHECK(optFailure->code == Error::Code::StorageFull);

    auto readTransaction = library.readTransaction();
    auto const optView = library.tracks().reader(readTransaction).get(trackId);
    REQUIRE(optView);
    CHECK(optView->metadata().title() == "Original");
    CHECK(optView->property().uri() == "original.flac");
  }
} // namespace ao::library::test
