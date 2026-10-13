// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <runtime/library/TrackBuilderSnapshot.h>

#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/MusicLibraryTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include "test/unit/runtime/library/ScanApplyTestSupport.h"
#include <ao/Error.h>
#include <ao/PictureType.h>
#include <ao/library/Credits.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/RecordingDate.h>
#include <ao/library/ResourceLayout.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackWriter.h>
#include <ao/utility/Sha256.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace ao::rt::test
{
  TEST_CASE("TrackBuilderSnapshot - borrowed cover bytes become an observed descriptor",
            "[runtime][unit][track-builder-snapshot]")
  {
    auto const coverBytes = std::array{std::byte{0x12}, std::byte{0x34}, std::byte{0x56}};
    auto builder = library::TrackBuilder::makeEmpty();
    builder.coverArt().add(PictureType::FrontCover, std::span<std::byte const>{coverBytes});

    auto snapshotRes = TrackBuilderSnapshot::make(builder);
    REQUIRE(snapshotRes);
    auto rebuilt = snapshotRes->makeBuilder();
    REQUIRE(rebuilt.coverArt().entries().size() == 1);
    auto const* const observed =
      std::get_if<library::ObservedResourceDescriptor>(&rebuilt.coverArt().entries().front().source);

    REQUIRE(observed);
    CHECK(rebuilt.coverArt().entries().front().type == PictureType::FrontCover);
    CHECK(observed->descriptor.digest == utility::computeSha256(coverBytes));
    CHECK(observed->descriptor.byteLength == coverBytes.size());
  }

  TEST_CASE("TrackBuilderSnapshot - observed cover evidence survives the owning snapshot",
            "[runtime][unit][track-builder-snapshot]")
  {
    auto const coverBytes = std::array{std::byte{0x9A}, std::byte{0xBC}};
    auto const expected = library::ObservedResourceDescriptor{
      .descriptor =
        library::ResourceDescriptor{
          .digest = utility::computeSha256(coverBytes),
          .byteLength = static_cast<std::uint32_t>(coverBytes.size()),
        },
    };
    auto builder = library::TrackBuilder::makeEmpty();
    builder.coverArt().add(PictureType::BackCover, expected);

    auto const rebuilt = [&builder]
    {
      auto snapshotRes = TrackBuilderSnapshot::make(builder);
      REQUIRE(snapshotRes);
      return snapshotRes->makeBuilder();
    }();
    // Only cover evidence is owned by value; text still borrows from the retired snapshot.
    REQUIRE(rebuilt.coverArt().entries().size() == 1);
    auto const& rebuiltCover = rebuilt.coverArt().entries().front();
    auto const* const observed = std::get_if<library::ObservedResourceDescriptor>(&rebuiltCover.source);

    REQUIRE(observed);
    CHECK(rebuiltCover.type == PictureType::BackCover);
    CHECK(observed->descriptor.digest == expected.descriptor.digest);
    CHECK(observed->descriptor.byteLength == expected.descriptor.byteLength);
  }

  TEST_CASE("TrackBuilderSnapshot - owns raw credit text source order duplicates and partial dates after move",
            "[runtime][unit][track-builder-snapshot]")
  {
    auto snapshot = []
    {
      auto const name = std::string{"Ada"};
      auto const piano = std::string{"Piano"};
      auto const spacedRole = std::string{"Piano "};
      auto const bob = std::string{"Bob"};
      auto const soloist = std::string{"Ann"};
      auto const conductor = std::string{" Rene\u0301 "};
      auto const ensemble = std::string{"Band"};
      auto const entries = std::array{
        library::CreditView{.name = name, .role = piano},
        library::CreditView{.name = soloist, .kind = library::CreditKind::Soloist, .role = piano},
        library::CreditView{.name = name, .role = piano},
        library::CreditView{.name = conductor, .kind = library::CreditKind::Conductor},
        library::CreditView{.name = name, .role = spacedRole},
        library::CreditView{.name = ensemble, .kind = library::CreditKind::Ensemble},
        library::CreditView{.name = bob, .role = {}},
      };
      auto builder = library::TrackBuilder::makeEmpty();
      builder.metadata().year(1972).recordingDate({.year = 1981, .month = 5}).credits(entries);
      builder.property().uri("snapshot.flac");

      auto snapshotRes = TrackBuilderSnapshot::make(builder);
      REQUIRE(snapshotRes);
      return std::move(*snapshotRes);
    }();

    // Views are taken only after this move. Moving the owner does not keep
    // borrows into its previous storage alive.
    auto moved = std::move(snapshot);
    auto rebuilt = moved.makeBuilder();
    auto const& metadata = rebuilt.metadata();

    CHECK(metadata.year() == 1972);
    CHECK(metadata.recordingDate() == library::RecordingDate{.year = 1981, .month = 5, .day = 0});
    auto const expected = std::array{
      library::CreditView{.name = "Ada", .role = "Piano"},
      library::CreditView{.name = "Ann", .kind = library::CreditKind::Soloist, .role = "Piano"},
      library::CreditView{.name = "Ada", .role = "Piano"},
      library::CreditView{.name = " Rene\u0301 ", .kind = library::CreditKind::Conductor},
      library::CreditView{.name = "Ada", .role = "Piano "},
      library::CreditView{.name = "Band", .kind = library::CreditKind::Ensemble},
      library::CreditView{.name = "Bob"},
    };
    REQUIRE(metadata.credits().size() == expected.size());

    for (std::size_t index = 0; index < expected.size(); ++index)
    {
      CHECK(metadata.credits()[index].name == expected[index].name);
      CHECK(metadata.credits()[index].kind == expected[index].kind);
      CHECK(metadata.credits()[index].role == expected[index].role);
    }

    auto const temp = ao::test::TempDir{};
    auto ml = library::test::makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto transaction = library::test::writeTransaction(ml);
    auto const createRes =
      transaction.apply([&](library::LibraryWrite& write)
                        { return write.tracks().create(rebuilt, library::FileManifestBuilder::makeEmpty()); });
    REQUIRE(createRes);
    REQUIRE(transaction.commit());
    CHECK(storedTrackSpec(ml, *createRes).credits ==
          std::vector<library::Credit>{
            {.name = "René", .kind = library::CreditKind::Conductor},
            {.name = "Band", .kind = library::CreditKind::Ensemble},
            {.name = "Ann", .kind = library::CreditKind::Soloist, .role = "Piano"},
            {.name = "Ada", .role = "Piano"},
            {.name = "Ada", .role = "Piano"},
            {.name = "Ada", .role = "Piano"},
            {.name = "Bob"},
          });
  }

  TEST_CASE("TrackBuilderSnapshot - raw invalid credits survive source expiry but fail builder admission",
            "[runtime][unit][track-builder-snapshot]")
  {
    auto entry = library::CreditView{.name = "Valid"};

    SECTION("blank name")
    {
      entry.name = " \t\r\n\v\f";
    }

    SECTION("malformed name")
    {
      entry.name = "\xC0\xAF";
    }

    SECTION("malformed role")
    {
      entry.role = "\xED\xA0\x80";
    }

    SECTION("invalid kind")
    {
      entry.kind = static_cast<library::CreditKind>(255);
    }

    auto snapshot = [&]
    {
      auto const name = std::string{entry.name};
      auto const role = std::string{entry.role};
      auto const entries = std::array{library::CreditView{.name = "Earlier valid"},
                                      library::CreditView{.name = name, .kind = entry.kind, .role = role}};
      auto builder = library::TrackBuilder::makeEmpty();
      builder.metadata().credits(entries);
      builder.property().uri("invalid.flac");
      auto res = TrackBuilderSnapshot::make(builder);
      REQUIRE(res);
      return std::move(*res);
    }();
    auto rebuilt = snapshot.makeBuilder();
    REQUIRE(rebuilt.metadata().credits().size() == 2);
    CHECK(rebuilt.metadata().credits()[0].name == "Earlier valid");
    CHECK(rebuilt.metadata().credits()[1].name == entry.name);
    CHECK(rebuilt.metadata().credits()[1].kind == entry.kind);
    CHECK(rebuilt.metadata().credits()[1].role == entry.role);

    auto const temp = ao::test::TempDir{};
    auto ml = library::test::makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto transaction = library::test::writeTransaction(ml);
    auto const res =
      transaction.apply([&](library::LibraryWrite& write)
                        { return write.tracks().create(rebuilt, library::FileManifestBuilder::makeEmpty()); });
    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::InvalidInput);
    auto readTransaction = ml.readTransaction();
    CHECK(ml.tracks().reader(readTransaction).entryCount() == 0);
    CHECK_FALSE(ml.manifest().reader(readTransaction).get("invalid.flac"));
    CHECK(ml.dictionary().size() == 0);
  }
} // namespace ao::rt::test
