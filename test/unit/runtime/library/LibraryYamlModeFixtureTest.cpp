// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/library/LibraryYamlExporter.h"
#include "runtime/library/LibraryYamlImporter.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/MusicLibraryTestSupport.h"
#include <ao/AudioCodec.h>
#include <ao/CoreIds.h>
#include <ao/FileTimestamp.h>
#include <ao/library/Credits.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/ListStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/RecordingDate.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>
#include <ao/rt/library/LibraryTransfer.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The version-1 documents beside v1.yaml in test/data/library: one for each
// export mode other than full, and one of scalar edge cases.
//
// v1-delta.yaml and v1-metadata.yaml were drafted by hand, then restored with
// the Linux CLI into an empty directory <root> and replaced by their mode's
// export:
//
//   aobus -C <root> lib import v1-<mode>.yaml --mode restore --confirm-destructive-restore
//   aobus -C <root> lib export <root>/out.yaml --mode <mode>
//
// so the test reproduces each byte for byte. v1-list-only.yaml is written by
// hand in the shape of a list-only export; it ranks the Tracks of
// v1-metadata.yaml by URI, segments by ordinal. v1-scalars.yaml is written by
// hand in spellings the exporter does not produce: YAML implicit typing would
// read many of them as a number, a boolean, or null, and the format gives each
// field its meaning instead.
//
// The documents freeze at the first release. The test map of
// doc/reference/library/format/yaml.md records what this test compares once a
// later exporter writes other bytes than a frozen document holds.

namespace ao::rt::test
{
  using namespace ao::library;

  namespace
  {
    // The version these documents declare, which their import reports whatever
    // version the exporter currently writes.
    constexpr std::uint32_t kFixtureVersion = 1;

    std::filesystem::path fixtureRoot()
    {
      return std::filesystem::path{LIBRARY_TEST_DATA_DIR};
    }

    ImportReport restore(MusicLibrary& library, std::filesystem::path const& path)
    {
      auto const reportRes = LibraryYamlImporter{library}.importFromYamlOffline(path, ImportMode::Restore);
      REQUIRE(reportRes);
      return *reportRes;
    }

    std::string exportAs(MusicLibrary const& library, std::filesystem::path const& path, ExportMode const mode)
    {
      REQUIRE(LibraryYamlExporter{library}.exportToYaml(path, mode));
      return ao::test::readFile(path);
    }

    std::optional<TrackView> findTrack(MusicLibrary const& library,
                                       ReadTransaction const& transaction,
                                       std::string_view uri)
    {
      for (auto const& [trackId, view] : library.tracks().reader(transaction))
      {
        if (view.property().uri() == uri)
        {
          return view;
        }
      }

      return std::nullopt;
    }

    struct ListShape final
    {
      std::string name{};
      std::string parentName{};
      std::string description{};
      std::string filter{};
      // Each rank as its URI, with "#<ordinal>" for a segment.
      std::vector<std::string> order{};

      bool operator==(ListShape const&) const = default;
    };

    // Lists in id order, with parents and ranks resolved, so a restore that
    // assigns new ids keeps the same shapes.
    std::vector<ListShape> listShapes(MusicLibrary const& library)
    {
      auto transaction = library.readTransaction();
      auto const listReader = library.lists().reader(transaction);
      auto const trackReader = library.tracks().reader(transaction);
      auto shapes = std::vector<ListShape>{};

      for (auto const& [listId, view] : listReader)
      {
        auto shape = ListShape{.name = std::string{view.name()},
                               .description = std::string{view.description()},
                               .filter = std::string{view.filter()}};

        if (auto const optParent = listReader.get(view.parentId()); optParent)
        {
          shape.parentName = optParent->name();
        }

        for (auto const trackId : view.orderTrackIds())
        {
          auto const optTrack = trackReader.get(trackId);
          REQUIRE(optTrack);
          auto rank = std::string{optTrack->property().uri()};

          if (auto const optSegment = optTrack->property().segment(); optSegment)
          {
            rank += "#" + std::to_string(optSegment->ordinal);
          }

          shape.order.push_back(std::move(rank));
        }

        shapes.push_back(std::move(shape));
      }

      return shapes;
    }

    // Each scalar edge case of v1-scalars.yaml holds the meaning the format
    // gives it, checked on each library the document reaches.
    void checkScalarMeanings(MusicLibrary const& library, std::string_view libraryName)
    {
      INFO("library: " << libraryName);

      // The track checks' transaction ends before listShapes opens its own.
      {
        auto transaction = library.readTransaction();
        auto const& dictionary = library.dictionary();
        auto const text = [&dictionary](DictionaryId const id) { return dictionary.get(id); };

        auto const optText = findTrack(library, transaction, "Edge/01 Text.flac");
        REQUIRE(optText);
        auto const metadata = optText->metadata();
        // Text fields hold the scalar text, whatever YAML implicit typing reads
        // it as, and only the quoting is removed.
        CHECK(metadata.title() == "007");
        CHECK(text(metadata.artistId()) == "yes");
        CHECK(text(metadata.albumId()) == "null");
        CHECK(text(metadata.albumArtistId()) == "~");
        CHECK(text(metadata.composerId()) == "1e3");
        CHECK(text(metadata.genreId()) == "0x1F");
        CHECK(text(optText->work().workId()) == "true");
        CHECK(text(optText->work().movementId()) == "It's: #1");

        // A quoted empty recording-date is the clear form, not a date.
        CHECK_FALSE(optText->performance().recordingDate().isPresent());
        auto const credits = optText->performance().credits();
        REQUIRE(credits.size() == 2);
        CHECK(text(credits[0].nameId) == "NO");
        CHECK(static_cast<CreditKind>(credits[0].kind) == CreditKind::Conductor);
        // Literal ~ and null role text is descriptive text, not absence.
        CHECK(text(credits[0].roleId) == "~");
        CHECK(text(credits[1].nameId) == "off");
        CHECK(static_cast<CreditKind>(credits[1].kind) == CreditKind::Performer);
        CHECK(text(credits[1].roleId) == "null");

        auto custom = std::map<std::string, std::string, std::less<>>{};

        for (auto const [keyId, value] : optText->customMetadata())
        {
          custom.emplace(text(keyId), value);
        }

        CHECK(custom == std::map<std::string, std::string, std::less<>>{{"3.14", "2.50"}, {"no", "off"}});
        // Codec text is case-insensitive.
        CHECK(optText->property().codec() == AudioCodec::Flac);

        // The decomposed spelling of a tag is canonically equivalent to the
        // precomposed one, so they are one member.
        auto tags = std::vector<std::string>{};

        for (auto const tagId : optText->tags())
        {
          tags.emplace_back(text(tagId));
        }

        std::ranges::sort(tags);
        CHECK(tags == std::vector<std::string>{"12", "Caf\u00e9", "on"});

        auto const manifestReader = library.manifest().reader(transaction);
        auto const optTextFile = manifestReader.get("Edge/01 Text.flac");
        REQUIRE(optTextFile);
        CHECK(optTextFile->fileSize() == 0);
        // An explicit null mtime records that no modification time is known.
        CHECK_FALSE(optTextFile->mtime());

        // Percent text in a URI is literal, so the dots are not a parent step.
        auto const optNumbers = findTrack(library, transaction, "Edge/%2e%2e/02 Numbers.flac");
        REQUIRE(optNumbers);
        // The document spells the title decomposed; Track text is stored in NFC.
        CHECK(optNumbers->metadata().title() == "Caf\u00e9 au lait");
        CHECK(optNumbers->metadata().year() == std::numeric_limits<std::uint16_t>::max());
        CHECK(optNumbers->metadata().trackNumber() == std::numeric_limits<std::uint16_t>::max());
        // An empty plain recording-date is the clear form too.
        CHECK_FALSE(optNumbers->performance().recordingDate().isPresent());
        CHECK(optNumbers->property().duration() == std::chrono::milliseconds{std::numeric_limits<std::int32_t>::max()});
        CHECK(optNumbers->property().bitrate() == std::numeric_limits<std::uint32_t>::max());
        CHECK(optNumbers->property().sampleRate() == std::numeric_limits<std::uint32_t>::max());
        CHECK(optNumbers->property().codec() == AudioCodec::Unknown);
        CHECK(optNumbers->property().channels() == std::numeric_limits<std::uint8_t>::max());
        CHECK(optNumbers->property().bitDepth() == std::numeric_limits<std::uint8_t>::max());

        auto const optNumbersFile = manifestReader.get("Edge/%2e%2e/02 Numbers.flac");
        REQUIRE(optNumbersFile);
        CHECK(optNumbersFile->fileSize() == std::numeric_limits<std::uint64_t>::max());
        REQUIRE(optNumbersFile->mtime());
        CHECK(*optNumbersFile->mtime() ==
              FileTimestamp{.seconds = std::numeric_limits<std::int64_t>::min(), .nanoseconds = 999999999});
      }

      // The scalar and id-map order references name the same records, and the
      // filter keeps its quotes byte for byte.
      CHECK(listShapes(library) ==
            std::vector<ListShape>{ListShape{.name = "2024",
                                             .description = "First line\nsecond line\n",
                                             .filter = "$genre = \"0x1F\"",
                                             .order = {"Edge/01 Text.flac", "Edge/%2e%2e/02 Numbers.flac"}}});
    }
  } // namespace

  TEST_CASE("Library YAML mode fixtures - the delta and metadata documents restore and export unchanged",
            "[runtime][integration][import-export][format-fixture]")
  {
    struct ModeFixture final
    {
      std::string_view file;
      ExportMode mode = ExportMode::Full;
      std::uint64_t trackCount = 0;
    };

    for (auto const& fixture : {ModeFixture{.file = "v1-delta.yaml", .mode = ExportMode::Delta, .trackCount = 3},
                                ModeFixture{.file = "v1-metadata.yaml", .mode = ExportMode::Metadata, .trackCount = 4}})
    {
      DYNAMIC_SECTION(fixture.file)
      {
        auto const temp = ao::test::TempDir{};
        auto library = library::test::makeTestMusicLibrary(temp.path(), temp.path());
        auto const yamlPath = fixtureRoot() / fixture.file;

        // The music root holds none of the audio files the document names, so
        // a delta document overlays empty media metadata.
        CHECK(restore(library, yamlPath) == ImportReport{.payloadVersion = kFixtureVersion,
                                                         .payloadMode = fixture.mode,
                                                         .tracksCreated = fixture.trackCount,
                                                         .listsCreated = 3});
        CHECK(exportAs(library, temp.path() / "export.yaml", fixture.mode) == ao::test::readFile(yamlPath));
      }
    }
  }

  TEST_CASE("Library YAML mode fixtures - the list-only document replaces Lists and ranks Tracks by URI",
            "[runtime][integration][import-export][format-fixture]")
  {
    auto const temp = ao::test::TempDir{};
    auto library = library::test::makeTestMusicLibrary(temp.path(), temp.path());
    restore(library, fixtureRoot() / "v1-metadata.yaml");

    CHECK(restore(library, fixtureRoot() / "v1-list-only.yaml") == ImportReport{.payloadVersion = kFixtureVersion,
                                                                                .payloadMode = ExportMode::ListOnly,
                                                                                .targetScope = ImportTargetScope::Lists,
                                                                                .listsCreated = 3,
                                                                                .listsDeleted = 3});

    auto const carrier = std::string{"Live/The Carrier Band/Live at the Hall.flac"};
    CHECK(listShapes(library) ==
          std::vector<ListShape>{
            ListShape{.name = "Evening",
                      .description = "Ranked by hand",
                      .order = {"Pop/Example Artist/Single/01 Example Song.mp3",
                                "Classical/Bach/Cello Suite No. 1/01 Pr\u00e9lude.flac"}},
            ListShape{.name = "Evening classical", .parentName = "Evening", .filter = "$genre = Classical"},
            ListShape{.name = "Encore first", .order = {carrier + "#2", carrier + "#1"}},
          });
  }

  TEST_CASE("Library YAML mode fixtures - each scalar spelling keeps the meaning its field gives it",
            "[runtime][integration][import-export][format-fixture]")
  {
    auto const temp = ao::test::TempDir{};
    auto const libraryRoot = temp.path() / "first";
    REQUIRE(std::filesystem::create_directory(libraryRoot));
    auto library = library::test::makeTestMusicLibrary(libraryRoot, libraryRoot);

    CHECK(restore(library, fixtureRoot() / "v1-scalars.yaml") ==
          ImportReport{.payloadVersion = kFixtureVersion, .tracksCreated = 2, .listsCreated = 1});

    checkScalarMeanings(library, "restored from v1-scalars.yaml");

    // The exporter writes these values without forcing quotes, and its
    // spellings read back with the same meaning; exporting that copy again
    // changes no byte.
    auto const exported = exportAs(library, temp.path() / "first.yaml", ExportMode::Full);
    auto const copyRoot = temp.path() / "copy";
    REQUIRE(std::filesystem::create_directory(copyRoot));
    auto copy = library::test::makeTestMusicLibrary(copyRoot, copyRoot);
    restore(copy, temp.path() / "first.yaml");
    checkScalarMeanings(copy, "restored from its export");
    CHECK(exportAs(copy, temp.path() / "copy.yaml", ExportMode::Full) == exported);
  }
} // namespace ao::rt::test
