// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/library/PhysicalStoreAccess.h"
#include "test/library/TrackStoreTestSupport.h"
#include "test/library/WritableLibraryTestSupport.h"
#include <ao/AudioScalars.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/PictureType.h>
#include <ao/library/Credits.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/ResourceLayout.h>
#include <ao/library/ResourceStore.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackLayout.h>
#include <ao/library/TrackView.h>
#include <ao/library/TrackWriter.h>
#include <ao/library/WriteTransaction.h>
#include <ao/utility/Sha256.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace ao::library::test
{
  namespace
  {
    using PreparedTrack = std::pair<TrackBuilder::PreparedHot, TrackBuilder::PreparedCold>;

    template<typename Preparation>
    concept HasPublicStagingFactory = requires(TrackBuilder const& builder, WriteTransaction& transaction) {
      Preparation::stageHotValidated(builder, transaction);
    } || requires(TrackBuilder const& builder, WriteTransaction& transaction, ResourceStore const& resources) {
      Preparation::stageColdValidated(builder, transaction, resources);
    } || requires(TrackBuilder const& builder, WriteTransaction& transaction, ResourceStore const& resources) {
      Preparation::stageValidated(builder, transaction, resources);
    } || requires(std::span<TrackBuilder const> tracks, WriteTransaction& transaction, ResourceStore const& resources) {
      Preparation::stageValidatedMembers(tracks, transaction, resources);
    };

    static_assert(!HasPublicStagingFactory<detail::TrackPreparation>);

    template<typename Prepared>
    std::vector<std::byte> serializedBytes(Prepared const& prepared)
    {
      auto bytes = std::vector<std::byte>(prepared.size());
      prepared.writeTo(bytes);
      return bytes;
    }

    ResourceId createCoverDeclaration(MusicLibrary& library, std::string_view uri, ResourceDescriptor const& descriptor)
    {
      auto builder = TrackBuilder::makeEmpty();
      builder.property().uri(uri);
      builder.coverArt().add(PictureType::FrontCover, descriptor);
      auto transaction = writeTransaction(library);
      auto const resourceIdRes = transaction.apply(
        [&builder](LibraryWrite& write) -> Result<ResourceId>
        {
          auto writer = write.tracks();
          auto const idRes = writer.create(builder, FileManifestBuilder::makeEmpty());
          REQUIRE(idRes);
          auto const optView = writer.get(*idRes);
          REQUIRE(optView);
          auto const optEntry = optView->coverArt().primary();
          REQUIRE(optEntry);
          return optEntry->resourceId;
        });
      REQUIRE(resourceIdRes);
      REQUIRE(transaction.commit());
      return *resourceIdRes;
    }
  } // namespace

  TEST_CASE("TrackPreparation - one-sided staging owns captured text")
  {
    auto fixture = TrackStoreFixture{};
    auto const& dictionary = fixture.library.dictionary();
    auto transaction = writeTransaction(fixture.library);
    auto builder = TrackBuilder::makeEmpty();
    builder.property().uri("track.flac");

    // Overwrite bytes without changing their lifetime or extent, so a mistakenly
    // retained view yields a deterministic wrong identity.
    SECTION("Hot only")
    {
      auto title = std::string{"Title"};
      auto artist = std::string{"Artist"};
      auto tag = std::string{"Tag"};
      builder.metadata().title(title).artist(artist);
      builder.tags().add(tag);
      auto const preparedRes = transaction.apply(
        [&](LibraryWrite& write) -> Result<TrackBuilder::PreparedHot>
        {
          REQUIRE(write.tracks().validate(builder));
          auto operation = detail::PhysicalStoreAccess::stageHotTrack(builder, transaction);
          static_assert(!std::is_default_constructible_v<decltype(operation)>);
          title.front() = 'X';
          artist.front() = 'X';
          tag.front() = 'X';
          return std::move(operation).finish();
        });
      REQUIRE(preparedRes);
      REQUIRE(transaction.commit());

      auto const bytes = serializedBytes(*preparedRes);
      auto const view = TrackView{bytes, {}};
      REQUIRE(view.isHotValid());
      CHECK(view.metadata().title() == "Title");
      CHECK(dictionary.getOrDefault(view.metadata().artistId()) == "Artist");
      REQUIRE(view.tags().count() == 1);
      CHECK(dictionary.getOrDefault(view.tags().id(0)) == "Tag");
    }

    SECTION("Cold only")
    {
      auto uri = std::string{"cold.flac"};
      auto key = std::string{"custom-key"};
      auto value = std::string{"Cafe\u0301"};
      auto credit = Credit{.name = "Soloist", .kind = CreditKind::Soloist, .role = "piano"};
      builder.property().uri(uri);
      builder.customMetadata().add(key, value);
      builder.metadata().credits(std::span<Credit const>{&credit, 1});
      auto const preparedRes = transaction.apply(
        [&](LibraryWrite& write) -> Result<TrackBuilder::PreparedCold>
        {
          REQUIRE(write.tracks().validate(builder));
          auto operation =
            detail::PhysicalStoreAccess::stageColdTrack(builder, transaction, fixture.library.resources());
          static_assert(!std::is_default_constructible_v<decltype(operation)>);
          uri.front() = 'x';
          key.front() = 'x';
          value.front() = 'X';
          credit.name.front() = 'X';
          credit.role.front() = 'x';
          return std::move(operation).finish();
        });
      REQUIRE(preparedRes);
      REQUIRE(transaction.commit());

      auto const bytes = serializedBytes(*preparedRes);
      auto const view = TrackView{{}, bytes};
      REQUIRE(view.isColdValid());
      CHECK(view.property().uri() == "cold.flac");
      auto const optKeyId = dictionary.findId("custom-key");
      REQUIRE(optKeyId);
      auto const optStoredValue = view.customMetadata().get(*optKeyId);
      REQUIRE(optStoredValue);
      CHECK(*optStoredValue == "Café");
      CHECK_FALSE(dictionary.findId("Café"));
      CHECK_FALSE(dictionary.findId("Xafé"));
      auto const credits = view.performance().credits(CreditKind::Soloist);
      REQUIRE(credits.size() == 1);
      CHECK(dictionary.getOrDefault(credits.front().nameId) == "Soloist");
      CHECK(dictionary.getOrDefault(credits.front().roleId) == "piano");
    }
  }

  TEST_CASE("TrackPreparation - paired staging captures both halves and counts raw cover bytes")
  {
    auto fixture = TrackStoreFixture{};
    auto const& dictionary = fixture.library.dictionary();
    auto const& resources = fixture.library.resources();
    auto title = std::string{"Paired title"};
    auto uri = std::string{"paired.flac"};
    auto cover = std::array{std::byte{0x31}, std::byte{0x32}, std::byte{0x33}};
    auto const full = std::span<std::byte const>{cover};
    // Same address but different extents must yield different cover evidence.
    auto const prefix = full.first(2);
    auto const digest = utility::computeSha256(full);
    auto const prefixDigest = utility::computeSha256(prefix);
    constexpr std::uint32_t kDeclaredLength = 99;
    auto const resourceId = createCoverDeclaration(
      fixture.library, "declared.flac", ResourceDescriptor{.digest = digest, .byteLength = kDeclaredLength});
    {
      auto const read = fixture.library.readTransaction();
      auto const optDescriptor = resources.reader(read).get(resourceId);
      REQUIRE(optDescriptor);
      REQUIRE(optDescriptor->byteLength == kDeclaredLength);
    }

    auto builder = TrackBuilder::makeEmpty();
    builder.metadata().title(title).artist("Paired artist");
    builder.property().uri(uri);
    builder.coverArt()
      .add(PictureType::FrontCover, full)
      .add(PictureType::BackCover, prefix)
      .add(PictureType::Media, full);
    auto transaction = writeTransaction(fixture.library);
    auto const preparedRes = transaction.apply(
      [&](LibraryWrite& write) -> Result<PreparedTrack>
      {
        REQUIRE(write.tracks().validate(builder));
        auto operation = detail::PhysicalStoreAccess::stageTrack(builder, transaction, resources);
        title.front() = 'X';
        uri.front() = 'x';
        cover.front() = std::byte{0xFF};
        return std::move(operation).finish();
      });
    REQUIRE(preparedRes);
    REQUIRE(transaction.commit());

    auto const hotBytes = serializedBytes(preparedRes->first);
    auto const coldBytes = serializedBytes(preparedRes->second);
    auto const view = TrackView{hotBytes, coldBytes};
    REQUIRE(view.isHotValid());
    REQUIRE(view.isColdValid());
    CHECK(view.metadata().title() == "Paired title");
    CHECK(dictionary.getOrDefault(view.metadata().artistId()) == "Paired artist");
    CHECK(view.property().uri() == "paired.flac");
    auto const covers = view.coverArt();
    REQUIRE(covers.count() == 3);
    CHECK(covers.at(0).resourceId == resourceId);
    CHECK(covers.at(2).resourceId == resourceId);
    auto const read = fixture.library.readTransaction();
    auto const reader = resources.reader(read);
    auto const optDescriptor = reader.get(resourceId);
    REQUIRE(optDescriptor);
    CHECK(optDescriptor->digest == digest);
    CHECK(optDescriptor->byteLength == full.size());
    auto const optPrefix = reader.get(covers.at(1).resourceId);
    REQUIRE(optPrefix);
    CHECK(optPrefix->digest == prefixDigest);
    CHECK(optPrefix->byteLength == prefix.size());
  }

  TEST_CASE("TrackPreparation - discarding a staged track leaves dictionary and Resources unchanged")
  {
    auto fixture = TrackStoreFixture{};
    auto const& resources = fixture.library.resources();
    auto const declaredCover = std::array{std::byte{0x31}, std::byte{0x32}, std::byte{0x33}};
    auto const newCover = std::array{std::byte{0x41}, std::byte{0x42}};
    constexpr std::uint32_t kDeclaredLength = 99;
    auto const resourceId = createCoverDeclaration(
      fixture.library,
      "declared.flac",
      ResourceDescriptor{.digest = utility::computeSha256(declaredCover), .byteLength = kDeclaredLength});

    auto builder = TrackBuilder::makeEmpty();
    builder.property().uri("discarded.flac");
    builder.metadata().artist("Discarded artist");
    builder.coverArt().add(PictureType::FrontCover, declaredCover).add(PictureType::BackCover, newCover);
    auto transaction = writeTransaction(fixture.library);
    REQUIRE(transaction.apply(
      [&](LibraryWrite& write) -> Result<>
      {
        REQUIRE(write.tracks().validate(builder));
        [[maybe_unused]] auto discarded = detail::PhysicalStoreAccess::stageTrack(builder, transaction, resources);
        return {};
      }));
    // Commit so leaked writes cannot be hidden by rollback.
    REQUIRE(transaction.commit());

    auto const read = fixture.library.readTransaction();
    auto const reader = resources.reader(read);
    auto const optDescriptor = reader.get(resourceId);
    REQUIRE(optDescriptor);
    CHECK(optDescriptor->byteLength == kDeclaredLength);
    auto entry = reader.begin();
    REQUIRE(entry != reader.end());
    CHECK(++entry == reader.end());
    CHECK(fixture.library.dictionary().size() == 0);
  }

  TEST_CASE("TrackPreparation - stages all carrier members before writing dictionary or Resources")
  {
    auto fixture = TrackStoreFixture{};
    auto const& dictionary = fixture.library.dictionary();
    auto const& resources = fixture.library.resources();
    auto transaction = writeTransaction(fixture.library);
    auto const publishedIdRes =
      transaction.apply([](LibraryWrite& write) { return physicalDictionary(write).intern("Published album"); });
    REQUIRE(publishedIdRes);
    REQUIRE(transaction.commit());

    constexpr auto kFirstSegment = TrackSegment{.startFrame = 588, .frameCount = 588, .ordinal = 2};
    constexpr auto kSecondSegment = TrackSegment{.startFrame = 0, .frameCount = 588, .ordinal = 1};
    auto const cover = std::array{std::byte{0x31}, std::byte{0x32}};

    auto localId = kInvalidDictionaryId;
    auto markerId = kInvalidDictionaryId;
    transaction = writeTransaction(fixture.library);
    auto const preparedRes = transaction.apply(
      [&](LibraryWrite& write) -> Result<std::vector<PreparedTrack>>
      {
        auto const localIdRes = physicalDictionary(write).intern("Writer-local artist");
        REQUIRE(localIdRes);
        localId = *localIdRes;
        auto secondTitle = std::string{"Second title"};
        auto firstSpelling = std::string{"Cafe\u0301"};
        auto sameSpelling = std::string{"Cafe\u0301"};
        auto canonicalSpelling = std::string{"Café"};
        auto work = std::string{"Suite for two orchestras"};
        auto members = std::array{TrackBuilder::makeEmpty(), TrackBuilder::makeEmpty()};
        members[0].property().uri("disc.flac").sampleRate(SampleRate{44100}).segment(kFirstSegment);
        members[1].property().uri("disc.flac").sampleRate(SampleRate{44100}).segment(kSecondSegment);
        members[0]
          .metadata()
          .title("First title")
          .artist(firstSpelling)
          .album("Published album")
          .genre("Prélude")
          .work(work);
        members[0].coverArt().add(PictureType::FrontCover, cover);
        members[1]
          .metadata()
          .title(secondTitle)
          .artist("Writer-local artist")
          .album(sameSpelling)
          .genre(canonicalSpelling)
          .composer("Final composer")
          .work("Pre\u0301lude")
          .movement(work);
        auto writer = write.tracks();
        REQUIRE(writer.validate(members[0]));
        REQUIRE(writer.validate(members[1]));

        auto operation = detail::PhysicalStoreAccess::stageTrackMembers(members, transaction, resources);
        static_assert(!std::is_copy_constructible_v<decltype(operation)>);
        static_assert(!std::is_copy_assignable_v<decltype(operation)>);
        static_assert(!std::is_move_constructible_v<decltype(operation)>);
        static_assert(!std::is_move_assignable_v<decltype(operation)>);
        CHECK(resources.reader(write).maxKey() == kInvalidResourceId);
        // Both captured spellings of Prélude must now resolve to this intervening write.
        auto const markerIdRes = physicalDictionary(write).intern("Prélude");
        REQUIRE(markerIdRes);
        markerId = *markerIdRes;
        secondTitle.front() = 'X';
        firstSpelling.front() = 'X';
        sameSpelling.front() = 'Y';
        canonicalSpelling.front() = 'Z';
        work.front() = 'X';

        return std::move(operation).finish();
      });
    REQUIRE(preparedRes);
    REQUIRE(preparedRes->size() == 2);
    REQUIRE(transaction.commit());

    // First use walks hot then cold for each member in input order, not segment ordinal order.
    CHECK(markerId.raw() == localId.raw() + 1U);
    auto const artistId = DictionaryId{markerId.raw() + 1U};
    auto const workId = DictionaryId{markerId.raw() + 2U};
    auto const composerId = DictionaryId{markerId.raw() + 3U};
    CHECK(dictionary.findId("Prélude") == markerId);
    CHECK(dictionary.findId("Café") == artistId);
    CHECK(dictionary.findId("Suite for two orchestras") == workId);
    CHECK(dictionary.findId("Final composer") == composerId);
    CHECK(dictionary.size() == composerId.raw());

    // Consume the prepared records after both the source storage and the operation are gone.
    auto hotBytes = std::array<std::vector<std::byte>, 2>{};
    auto coldBytes = std::array<std::vector<std::byte>, 2>{};

    for (std::size_t index = 0; index < preparedRes->size(); ++index)
    {
      auto const& [hot, cold] = (*preparedRes)[index];
      hotBytes[index] = serializedBytes(hot);
      coldBytes[index] = serializedBytes(cold);
    }

    auto const first = TrackView{hotBytes[0], coldBytes[0]};
    auto const second = TrackView{hotBytes[1], coldBytes[1]};
    REQUIRE(first.isHotValid());
    REQUIRE(first.isColdValid());
    REQUIRE(second.isHotValid());
    REQUIRE(second.isColdValid());
    CHECK(first.metadata().title() == "First title");
    CHECK(first.property().segment() == kFirstSegment);
    CHECK(first.metadata().artistId() == artistId);
    CHECK(first.metadata().albumId() == *publishedIdRes);
    CHECK(first.metadata().genreId() == markerId);
    CHECK(first.work().workId() == workId);
    CHECK(second.metadata().title() == "Second title");
    CHECK(second.property().segment() == kSecondSegment);
    CHECK(second.metadata().artistId() == localId);
    CHECK(second.metadata().albumId() == artistId);
    CHECK(second.metadata().genreId() == artistId);
    CHECK(second.metadata().composerId() == composerId);
    CHECK(second.work().workId() == markerId);
    CHECK(second.work().movementId() == workId);
    auto const optCover = first.coverArt().primary();
    REQUIRE(optCover);
    auto const read = fixture.library.readTransaction();
    auto const optDescriptor = resources.reader(read).get(optCover->resourceId);
    REQUIRE(optDescriptor);
    CHECK(optDescriptor->digest == utility::computeSha256(cover));
  }
} // namespace ao::library::test
