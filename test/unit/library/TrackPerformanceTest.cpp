// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/TrackBuilderTestSupport.h"
#include "test/unit/library/TrackStoreTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/PictureType.h>
#include <ao/library/Credits.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/RecordingDate.h>
#include <ao/library/ResourceStore.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackLayout.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>
#include <ao/library/TrackWriter.h>
#include <ao/library/WritableMusicLibrary.h>
#include <ao/library/WriteTransaction.h>
#include <ao/library/detail/TrackColdReader.h>
#include <ao/utility/ByteView.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ao::library::test
{
  TEST_CASE("TrackBuilder - work and performance blocks are independently optional",
            "[library][unit][track-performance]")
  {
    auto context = TrackSerializationFixture{};
    auto builder = TrackBuilder::makeEmpty();
    builder.property().uri("x");
    bool hasWork = false;
    bool hasPerformance = false;
    std::size_t entryCount = 0;
    auto const credits = std::array{CreditView{.name = "Glenn Gould", .kind = CreditKind::Soloist, .role = "piano"}};

    SECTION("empty domains")
    {
    }

    SECTION("work only")
    {
      builder.metadata().work("Goldberg Variations").movementNumber(1).movementTotal(30);
      hasWork = true;
    }

    SECTION("movement numbers alone")
    {
      builder.metadata().movementTotal(30);
      hasWork = true;
    }

    SECTION("date only")
    {
      builder.metadata().recordingDate(RecordingDate{.year = 1955});
      hasPerformance = true;
    }

    SECTION("credits only")
    {
      builder.metadata().credits(credits);
      hasPerformance = true;
      entryCount = 1;
    }

    SECTION("both domains")
    {
      builder.metadata().work("Goldberg Variations").credits(credits);
      hasWork = true;
      hasPerformance = true;
      entryCount = 1;
    }

    auto const cold = context.serializeCold(builder);
    auto const reader = detail::TrackColdReader{cold};
    REQUIRE(reader.isValid());
    auto const& header = reader.header();
    CHECK(reader.work().empty() == !hasWork);
    CHECK(reader.performance().empty() == !hasPerformance);
    CHECK(reader.performance().credits().size() == entryCount);
    CHECK(header.blockOffsets[4] == 0);
    CHECK(header.uriOffset ==
          sizeof(TrackColdHeader) + (hasWork ? 12 : 0) + (hasPerformance ? 12 + (8 * entryCount) : 0));
    CHECK((header.blockOffsets[1] != 0) == hasWork);
    CHECK((header.blockOffsets[3] != 0) == hasPerformance);
  }

  TEST_CASE("TrackBuilder - direct borrowed credit admission groups kinds and preserves duplicates roles and NFC",
            "[library][unit][track-performance]")
  {
    auto context = TrackSerializationFixture{};
    auto builder = TrackBuilder::makeEmpty();
    auto const entries = std::array{CreditView{.name = " \tCafe\u0301\r\n", .role = "\vpiano\f "},
                                    CreditView{.name = " Curated soloist ", .kind = CreditKind::Soloist},
                                    CreditView{.name = "Café", .role = " \t\r\n\f\v"},
                                    CreditView{.name = "  Conductor  ", .kind = CreditKind::Conductor},
                                    CreditView{.name = "Café", .role = "piano"},
                                    CreditView{.name = " Ensemble ", .kind = CreditKind::Ensemble},
                                    CreditView{.name = "Other", .role = "piano"}};
    builder.metadata().credits(entries);
    builder.property().uri("track.flac");
    auto const [hot, cold] = context.serialize(builder);
    auto const performance = TrackView{hot, cold}.performance();
    auto const performers = performance.credits(CreditKind::Performer);
    REQUIRE(performers.size() == 4);
    CHECK(context.dictionary().get(performers[0].nameId) == "Café");
    CHECK(context.dictionary().get(performers[0].roleId) == "piano");
    CHECK(performers[1].nameId == performers[0].nameId);
    CHECK(performers[1].roleId == kInvalidDictionaryId);
    CHECK(performers[2].nameId == performers[0].nameId);
    CHECK(performers[2].roleId == performers[0].roleId);
    CHECK(context.dictionary().get(performers[3].nameId) == "Other");
    CHECK(performers[3].roleId == performers[0].roleId);
    REQUIRE(performance.credits(CreditKind::Soloist).size() == 1);
    REQUIRE(performance.credits(CreditKind::Conductor).size() == 1);
    REQUIRE(performance.credits(CreditKind::Ensemble).size() == 1);
    CHECK(context.dictionary().get(performance.credits(CreditKind::Soloist)[0].nameId) == "Curated soloist");
    CHECK(context.dictionary().get(performance.credits(CreditKind::Conductor)[0].nameId) == "Conductor");
    CHECK(context.dictionary().get(performance.credits(CreditKind::Ensemble)[0].nameId) == "Ensemble");
    CHECK_FALSE(context.dictionary().findId("Cafe\u0301"));
    auto const* header = utility::layout::view<TrackColdHeader>(cold);
    CHECK(utility::bytes::view(performance.credits()).data() ==
          cold.data() + header->blockOffsets[3] + sizeof(TrackPerformanceBlock));
  }

  TEST_CASE("TrackBuilder - recording date retains exact partial precision independently of release year",
            "[library][unit][track-performance]")
  {
    auto context = TrackSerializationFixture{};

    for (auto const date : std::array{RecordingDate{},
                                      RecordingDate{.year = 1981},
                                      RecordingDate{.year = 1981, .month = 5},
                                      RecordingDate{.year = 2000, .month = 2, .day = 29}})
    {
      CAPTURE(date.year, date.month, date.day);
      auto builder = TrackBuilder::makeEmpty();
      builder.metadata().year(2024).recordingDate(date);
      builder.property().uri("date.flac");
      auto const [hot, cold] = context.serialize(builder);
      auto const view = TrackView{hot, cold};
      CHECK(view.performance().recordingDate() == date);
      CHECK(view.metadata().year() == 2024);
      CHECK(view.performance().empty() == !date.isPresent());
      CHECK(view.work().empty());
    }
  }

  TEST_CASE("TrackBuilder - complete reconstruction retains all kinds and hot reconstruction avoids cold",
            "[library][unit][track-performance]")
  {
    auto context = TrackSerializationFixture{};
    auto original = TrackBuilder::makeEmpty();
    auto const entries = std::array{CreditView{.name = "Conductor", .kind = CreditKind::Conductor},
                                    CreditView{.name = "Ensemble", .kind = CreditKind::Ensemble},
                                    CreditView{.name = "Soloist", .kind = CreditKind::Soloist},
                                    CreditView{.name = "A", .role = "piano"},
                                    CreditView{.name = "B"},
                                    CreditView{.name = "A", .role = "piano"}};
    auto const date = RecordingDate{.year = 1981, .month = 5};
    original.metadata()
      .title("Title")
      .work("Work")
      .movement("Movement")
      .movementNumber(2)
      .movementTotal(4)
      .recordingDate(date)
      .credits(entries);
    original.customMetadata().add("key", "value");
    original.property().uri("track.flac");
    auto const [hot, cold] = context.serialize(original);
    auto reconstructed = TrackBuilder::fromCompleteView(TrackView{hot, cold}, context.dictionary());
    CHECK(reconstructed.metadata().recordingDate() == date);
    REQUIRE(reconstructed.metadata().credits().size() == entries.size());

    for (std::size_t index = 0; index < entries.size(); ++index)
    {
      CHECK(reconstructed.metadata().credits()[index].name == entries[index].name);
      CHECK(reconstructed.metadata().credits()[index].kind == entries[index].kind);
      CHECK(reconstructed.metadata().credits()[index].role == entries[index].role);
    }

    auto const [rebuiltHot, rebuiltCold] = context.serialize(reconstructed);
    CHECK(rebuiltHot == hot);
    CHECK(rebuiltCold == cold);
    auto const invalidCold = std::array<std::byte, 1>{};
    auto hotOnly = TrackBuilder::fromHotView(TrackView{hot, invalidCold}, context.dictionary());
    CHECK(hotOnly.metadata().title() == "Title");
    CHECK(hotOnly.metadata().credits().empty());
    CHECK(hotOnly.metadata().recordingDate() == RecordingDate{});
    CHECK(hotOnly.metadata().work().empty());
  }

  TEST_CASE("TrackBuilder - whole-list replacement and clear preserve the independent date",
            "[library][unit][track-performance]")
  {
    auto context = TrackSerializationFixture{};
    auto builder = TrackBuilder::makeEmpty();
    auto const oldEntries = std::array{CreditView{.name = "Old", .kind = CreditKind::Soloist, .role = "violin"}};
    auto const replacement = std::array{CreditView{.name = "New"}, CreditView{.name = "New", .role = "piano"}};
    auto const date = RecordingDate{.year = 1981};
    builder.metadata().recordingDate(date).credits(oldEntries).credits(replacement);
    builder.property().uri("track.flac");
    auto const cold = context.serializeCold(builder);
    auto const performance = TrackView{{}, cold}.performance();
    REQUIRE(performance.credits().size() == 2);
    CHECK(context.dictionary().get(performance.credits()[0].nameId) == "New");
    CHECK(performance.credits()[0].roleId == kInvalidDictionaryId);
    CHECK(context.dictionary().get(performance.credits()[1].roleId) == "piano");
    CHECK(performance.credits(CreditKind::Soloist).empty());
    CHECK_FALSE(context.dictionary().findId("Old"));
    builder.metadata().credits(std::span<CreditView const>{});
    auto const cleared = context.serializeCold(builder);
    auto const clearedPerformance = TrackView{{}, cleared}.performance();
    CHECK(clearedPerformance.credits().empty());
    CHECK(clearedPerformance.recordingDate() == date);
  }

  TEST_CASE("TrackBuilder - invalid performance input rejects before staging dictionary or resources",
            "[library][unit][track-performance]")
  {
    auto context = TrackSerializationFixture{};
    auto builder = TrackBuilder::makeEmpty();
    builder.metadata().artist("Must not intern");
    builder.property().uri("track.flac");
    auto const cover = std::array{std::byte{1}, std::byte{2}};
    builder.coverArt().add(PictureType::FrontCover, cover);
    auto const malformed = std::string{"\xC0\xAF", 2};
    auto entry = CreditView{.name = "Name", .role = "Role"};

    SECTION("blank name")
    {
      entry.name = " \t\r\n\f\v";
    }

    SECTION("invalid name UTF-8")
    {
      entry.name = malformed;
    }

    SECTION("invalid role UTF-8")
    {
      entry.role = malformed;
    }

    SECTION("invalid kind")
    {
      entry.kind = static_cast<CreditKind>(255);
    }

    SECTION("invalid calendar date")
    {
      builder.metadata().recordingDate(RecordingDate{.year = 1900, .month = 2, .day = 29});
    }

    builder.metadata().credits(std::span<CreditView const>{&entry, 1});
    auto const checkRejected = [&](auto const& res)
    {
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::InvalidInput);
      CHECK(context.resources().reader(context.transaction()).maxKey() == kInvalidResourceId);
    };
    checkRejected(physicalSerializeTrack(builder, context.transaction(), context.resources()));
    checkRejected(physicalSerializeColdTrack(builder, context.transaction(), context.resources()));
    checkRejected(physicalPrepareTrack(builder, context.transaction(), context.resources()));
    checkRejected(physicalPrepareColdTrack(builder, context.transaction(), context.resources()));
    REQUIRE(context.transaction().commit());
    CHECK(context.dictionary().size() == 0);
  }

  TEST_CASE("TrackBuilder - aggregate cold bounds reject excessive credits and merged retained blocks before interning",
            "[library][unit][track-performance]")
  {
    auto context = TrackSerializationFixture{};
    auto builder = TrackBuilder::makeEmpty();
    builder.metadata().artist("Do not intern");
    builder.property().uri("x");
    std::size_t count = 8186;

    SECTION("prefix and tail exceed payload width")
    {
      count = std::numeric_limits<std::uint16_t>::max() / sizeof(TrackCreditEntry);
    }

    SECTION("aggregate record exceeds width despite fitting payload")
    {
    }

    SECTION("retained custom values make a small incoming list overflow")
    {
      count = 1;
      builder.customMetadata().add("key", std::string_view{});
    }

    auto const retained = std::string(count == 1 ? 65480 : 0, 'x');

    if (count == 1)
    {
      builder.customMetadata().clear().add("key", retained);
    }

    auto const entries = std::vector<CreditView>(count, CreditView{.name = "Do not intern name", .role = "Role"});
    builder.metadata().credits(entries);
    auto const res = physicalSerializeTrack(builder, context.transaction(), context.resources());
    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::ValueTooLarge);
    REQUIRE(context.transaction().commit());
    CHECK(context.dictionary().size() == 0);
  }

  TEST_CASE("TrackBuilder - largest credit segment fitting a short URI is stored without truncation",
            "[library][unit][track-performance]")
  {
    auto context = TrackSerializationFixture{};
    auto builder = TrackBuilder::makeEmpty();
    auto const entries = std::vector<CreditView>(8185, CreditView{.name = "Name", .kind = CreditKind::Conductor});
    builder.metadata().credits(entries);
    builder.property().uri("x");
    auto const cold = context.serializeCold(builder);
    CHECK(cold.size() == 65528);
    auto const reader = detail::TrackColdReader{cold};
    REQUIRE(reader.isValid());
    auto const credits = reader.performance().credits(CreditKind::Conductor);
    REQUIRE(credits.size() == 8185);
    CHECK(reader.performance().credits(CreditKind::Performer).empty());
    CHECK(context.dictionary().get(credits.front().nameId) == "Name");
    CHECK(credits.back().nameId == credits.front().nameId);
    CHECK(credits.back().roleId == kInvalidDictionaryId);
  }

  TEST_CASE("TrackBuilder - prepared performance survives owning input destruction and builder replacement",
            "[library][unit][track-performance]")
  {
    auto context = TrackSerializationFixture{};
    auto builder = TrackBuilder::makeEmpty();
    auto const date = RecordingDate{.year = 1981, .month = 5, .day = 12};
    bool coldOnly = false;

    SECTION("complete preparation")
    {
    }

    SECTION("cold-only preparation")
    {
      coldOnly = true;
    }

    auto preparedRes = [&] -> Result<TrackBuilder::PreparedCold>
    {
      auto const entries = std::vector<Credit>{{.name = " \tCafe\u0301\r\n", .role = " Pianoforte\u0301 "},
                                               {.name = "Other", .role = " \t\r\n\f\v"},
                                               {.name = " \tCafe\u0301\r\n", .role = " Pianoforte\u0301 "}};
      builder.metadata().recordingDate(date).credits(entries);
      builder.property().uri("snapshot.flac");

      if (coldOnly)
      {
        return physicalPrepareColdTrack(builder, context.transaction(), context.resources());
      }

      auto res = physicalPrepareTrack(builder, context.transaction(), context.resources());

      if (!res)
      {
        return std::unexpected{res.error()};
      }

      return std::move(res->second);
    }();
    REQUIRE(preparedRes);
    builder.metadata().credits(std::span<CreditView const>{}).recordingDate({});
    builder.property().uri("replacement.flac");
    auto bytes = std::vector<std::byte>(preparedRes->size(), std::byte{0xFF});
    preparedRes->writeTo(bytes);
    auto repeated = std::vector<std::byte>(preparedRes->size(), std::byte{0xAA});
    preparedRes->writeTo(repeated);
    CHECK(bytes == repeated);
    CHECK(context.dictionary().size() == 0);
    REQUIRE(context.transaction().commit());
    auto const reader = detail::TrackColdReader{bytes};
    REQUIRE(reader.isValid());
    CHECK(reader.uri() == "snapshot.flac");
    CHECK(reader.performance().recordingDate() == date);
    auto const credits = reader.performance().credits();
    REQUIRE(credits.size() == 3);
    CHECK(context.dictionary().get(credits[0].nameId) == "Café");
    CHECK(context.dictionary().get(credits[0].roleId) == "Pianoforté");
    CHECK(context.dictionary().get(credits[1].nameId) == "Other");
    CHECK(credits[1].roleId == kInvalidDictionaryId);
    CHECK(credits[2].nameId == credits[0].nameId);
    CHECK(credits[2].roleId == credits[0].roleId);
  }

  TEST_CASE("TrackWriter - rejected performance admission leaves no rows even when the operation commits",
            "[library][unit][track-performance]")
  {
    auto fixture = TrackStoreFixture{};
    auto rejected = TrackBuilder::makeEmpty();
    auto const entries = std::array{CreditView{.name = "   ", .role = "piano"}};
    auto const cover = std::array{std::byte{1}, std::byte{2}};
    rejected.metadata().artist("Must not intern").credits(entries);
    rejected.coverArt().add(PictureType::FrontCover, cover);
    rejected.property().uri("rejected.flac");
    auto accepted = TrackBuilder::makeEmpty();
    accepted.property().uri("accepted.flac");
    auto transaction = writeTransaction(fixture.library);
    auto acceptedRes = transaction.apply(
      [&](LibraryWrite& write) -> Result<TrackId>
      {
        auto writer = write.tracks();
        auto const rejectedRes = writer.create(rejected, FileManifestBuilder::makeEmpty());
        REQUIRE_FALSE(rejectedRes);
        CHECK(rejectedRes.error().code == Error::Code::InvalidInput);
        CHECK_FALSE(writer.manifest("rejected.flac"));
        // Success after preflight rejection proves no preparation effects require rollback.
        return writer.create(accepted, FileManifestBuilder::makeEmpty());
      });
    REQUIRE(acceptedRes);
    REQUIRE(transaction.commit());
    CHECK(*acceptedRes == TrackId{1});
    CHECK(fixture.library.dictionary().size() == 0);
    auto read = fixture.library.readTransaction();
    auto const reader = fixture.library.tracks().reader(read);
    CHECK(reader.entryCount() == 1);
    auto const optView = reader.get(*acceptedRes);
    REQUIRE(optView);
    CHECK(optView->property().uri() == "accepted.flac");
    CHECK_FALSE(fixture.library.manifest().reader(read).get("rejected.flac"));
    CHECK(fixture.library.resources().reader(read).maxKey() == kInvalidResourceId);
  }

  TEST_CASE("TrackWriter - rejected performance updates remain write-neutral when committed",
            "[library][unit][track-performance]")
  {
    auto fixture = TrackStoreFixture{};
    auto const spec =
      TrackSpec{.title = "Before",
                .recordingDate = RecordingDate{.year = 1981},
                .credits = {{.name = "Soloist", .kind = CreditKind::Soloist}, {.name = "Original", .role = "piano"}}};
    auto const id = addTrack(fixture.library, spec);
    auto const dictionarySize = fixture.library.dictionary().size();
    bool coldOnly = false;

    SECTION("complete update")
    {
    }

    SECTION("cold-only update")
    {
      coldOnly = true;
    }

    auto transaction = writeTransaction(fixture.library);
    REQUIRE(transaction.apply(
      [&](LibraryWrite& write) -> Result<>
      {
        auto writer = write.tracks();
        auto const optView = writer.get(id);
        REQUIRE(optView);
        auto builder = TrackBuilder::fromCompleteView(*optView, fixture.library.dictionary());
        auto const entries = std::array{
          CreditView{.name = "Must not intern", .role = "piano"}, CreditView{.name = "Valid", .role = "\xC0\xAF"}};
        auto const cover = std::array{std::byte{1}, std::byte{2}};
        builder.metadata().title("After").artist("Must not intern artist").credits(entries);
        builder.coverArt().add(PictureType::FrontCover, cover);
        auto const res = coldOnly ? writer.updateCold(id, builder) : writer.update(id, builder);
        REQUIRE_FALSE(res);
        CHECK(res.error().code == Error::Code::InvalidInput);
        return {};
      }));
    REQUIRE(transaction.commit());
    CHECK(fixture.library.dictionary().size() == dictionarySize);
    auto read = fixture.library.readTransaction();
    auto const reader = fixture.library.tracks().reader(read);
    CHECK(reader.entryCount() == 1);
    auto const optView = reader.get(id);
    REQUIRE(optView);
    auto const stored = trackSpecFromView(fixture.library, *optView);
    CHECK(stored.title == spec.title);
    CHECK(stored.artist == spec.artist);
    CHECK(stored.recordingDate == spec.recordingDate);
    CHECK(stored.credits == spec.credits);
    CHECK(stored.uri == spec.uri);
    CHECK(optView->coverArt().count() == 0);
    CHECK(fixture.library.resources().reader(read).maxKey() == kInvalidResourceId);
    auto const optManifest = fixture.library.manifest().reader(read).get(spec.uri);
    REQUIRE(optManifest);
    CHECK(optManifest->trackId() == id);
  }

  TEST_CASE("TrackWriter - hot updates preserve owning fixture credits and complete updates clear them",
            "[library][unit][track-performance]")
  {
    auto fixture = TrackStoreFixture{};
    auto const spec = TrackSpec{.title = "Before",
                                .recordingDate = RecordingDate{.year = 1981},
                                .credits = {{.name = "Soloist", .kind = CreditKind::Soloist},
                                            {.name = "A", .role = "piano"},
                                            {.name = "B"},
                                            {.name = "A", .role = "piano"}}};
    auto const id = addTrack(fixture.library, spec);
    auto transaction = writeTransaction(fixture.library);
    REQUIRE(transaction.apply(
      [&](LibraryWrite& write) -> Result<>
      {
        auto writer = write.tracks();
        auto const optView = writer.get(id, TrackStore::Reader::LoadMode::Hot);
        REQUIRE(optView);
        auto builder = TrackBuilder::fromHotView(*optView, fixture.library.dictionary());
        builder.metadata().title("After");
        return writer.updateHot(id, builder);
      }));
    REQUIRE(transaction.commit());

    {
      auto read = fixture.library.readTransaction();
      auto const optView = fixture.library.tracks().reader(read).get(id);
      REQUIRE(optView);
      auto const stored = trackSpecFromView(fixture.library, *optView);
      CHECK(stored.title == "After");
      CHECK(stored.recordingDate == spec.recordingDate);
      CHECK(stored.credits == spec.credits);
    }

    updateTrackSpec(fixture.library, id, [](TrackSpec& value) { value.credits.clear(); });

    {
      auto read = fixture.library.readTransaction();
      auto const optView = fixture.library.tracks().reader(read).get(id);
      REQUIRE(optView);
      CHECK(optView->performance().credits().empty());
      CHECK(optView->performance().recordingDate() == spec.recordingDate);
    }

    updateTrackSpec(fixture.library, id, [](TrackSpec& value) { value.recordingDate = {}; });
    auto read = fixture.library.readTransaction();
    auto const optView = fixture.library.tracks().reader(read).get(id);
    REQUIRE(optView);
    CHECK(optView->performance().empty());
    CHECK(optView->performance().recordingDate() == RecordingDate{});
  }

  TEST_CASE("TrackWriter - native failure after credit interning aborts all staged text and rows",
            "[library][unit][track-performance]")
  {
    constexpr std::size_t kMapBytes = std::size_t{256} * 1024;
    auto const temp = ao::test::TempDir{};
    auto libraryRes =
      MusicLibrary::open(temp.path(), temp.path() / "db", MusicLibrary::Options{.pinnedMapBytes = kMapBytes});
    REQUIRE(libraryRes);
    auto& library = *libraryRes;
    auto writableRes = WritableMusicLibrary::acquire(library);
    REQUIRE(writableRes);
    auto transaction = writableRes->writeTransaction();
    auto const oversized = std::string(kMapBytes * 4, 'x');
    auto const entries =
      std::array{CreditView{.name = "Staged conductor", .kind = CreditKind::Conductor}, CreditView{.name = oversized}};
    auto builder = TrackBuilder::makeEmpty();
    builder.metadata().credits(entries);
    builder.property().uri("failure.flac");
    bool returnedFromCreate = false;
    auto const operationRes = transaction.apply(
      [&](LibraryWrite& write) -> Result<>
      {
        auto const createRes = write.tracks().create(builder, FileManifestBuilder::makeEmpty());
        returnedFromCreate = true;
        // A post-intern error must not be catchable as a per-call Result permitting commit.
        CHECK(createRes);
        return {};
      });
    REQUIRE_FALSE(operationRes);
    CHECK(operationRes.error().code == Error::Code::StorageFull);
    CHECK_FALSE(returnedFromCreate);
    CHECK(library.dictionary().size() == 0);
    CHECK_FALSE(library.dictionary().findId("Staged conductor"));
    CHECK_FALSE(library.dictionary().findId(oversized));
    auto read = library.readTransaction();
    CHECK(library.tracks().reader(read).entryCount() == 0);
    CHECK_FALSE(library.manifest().reader(read).get("failure.flac"));
    CHECK(library.resources().reader(read).maxKey() == kInvalidResourceId);
    auto retry = writableRes->writeTransaction();
    retry.abort();
  }
} // namespace ao::library::test
