// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/library/TrackRecordValidation.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/MusicLibraryTestSupport.h"
#include "test/unit/library/TrackStoreTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include "test/unit/lmdb/LmdbTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/Credits.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/RecordingDate.h>
#include <ao/library/TrackLayout.h>
#include <ao/library/TrackView.h>
#include <ao/library/WriteTransaction.h>
#include <ao/library/detail/TrackColdReader.h>
#include <ao/utility/ByteView.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ao::library::test
{
  namespace
  {
    std::vector<std::byte> performanceRecord(TrackPerformanceBlock const& prefix,
                                             std::span<TrackCreditEntry const> entries = {})
    {
      constexpr auto kUri = std::string_view{"track.flac"};
      auto const uriOffset = sizeof(TrackColdHeader) + sizeof(prefix) + entries.size_bytes();
      auto bytes = std::vector<std::byte>(alignToWord(uriOffset + kUri.size()), std::byte{0});
      auto header = TrackColdHeader{
        .uriOffset = static_cast<std::uint16_t>(uriOffset), .uriLength = static_cast<std::uint16_t>(kUri.size())};
      header.blockOffsets[3] = sizeof(TrackColdHeader);
      std::memcpy(bytes.data(), &header, sizeof(header));
      std::memcpy(bytes.data() + sizeof(header), &prefix, sizeof(prefix));

      if (!entries.empty())
      {
        std::memcpy(bytes.data() + sizeof(header) + sizeof(prefix), entries.data(), entries.size_bytes());
      }

      std::memcpy(bytes.data() + uriOffset, kUri.data(), kUri.size());
      return bytes;
    }

    void checkPerformanceGateRejects(std::span<std::byte const> bytes)
    {
      CHECK_FALSE(TrackView{{}, bytes}.isColdValid());
      CHECK_FALSE(detail::TrackColdReader{bytes}.isValid());
      auto const res = validateSerializedColdTrack(bytes);
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::CorruptData);
    }

    void checkPersistedColdBytes(std::filesystem::path const& path, std::span<std::byte const> expected)
    {
      auto const optBytes = lmdb::test::readExistingIntegerKeyRecord(path, "tracks_cold", 1);
      REQUIRE(optBytes);
      CHECK(*optBytes == std::vector<std::byte>{expected.begin(), expected.end()});
    }
  } // namespace

  TEST_CASE("TrackView - performance gate leaves date and empty-block semantics to deep validation",
            "[library][unit][track-performance]")
  {
    for (auto const date : std::array{RecordingDate{},
                                      RecordingDate{.month = 1},
                                      RecordingDate{.day = 1},
                                      RecordingDate{.year = 10000},
                                      RecordingDate{.year = 1981, .month = 13},
                                      RecordingDate{.year = 1981, .day = 1},
                                      RecordingDate{.year = 1900, .month = 2, .day = 29},
                                      RecordingDate{.year = 2000, .month = 4, .day = 31}})
    {
      CAPTURE(date.year, date.month, date.day);
      auto const bytes = performanceRecord(TrackPerformanceBlock{.recordingDate = date});
      auto const view = TrackView{{}, bytes};
      REQUIRE(view.isColdValid());
      auto const performance = view.performance();
      CHECK_FALSE(performance.empty());
      CHECK(performance.recordingDate() == date);
      CHECK(performance.credits(CreditKind::Conductor).empty());
      CHECK(performance.credits(CreditKind::Ensemble).empty());
      CHECK(performance.credits(CreditKind::Soloist).empty());
      CHECK(performance.credits(CreditKind::Performer).empty());
      CHECK(performance.credits().empty());
      CHECK_FALSE(detail::TrackColdReader{bytes}.isValid());
      auto const res = validateSerializedColdTrack(bytes);
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::CorruptData);
    }
  }

  TEST_CASE("TrackView - performance gate borrows complete ordered credit tails", "[library][unit][track-performance]")
  {
    auto const entries = std::array{TrackCreditEntry{.nameId = DictionaryId{7}, .roleId = DictionaryId{3}},
                                    TrackCreditEntry{.nameId = DictionaryId{9}},
                                    TrackCreditEntry{.nameId = DictionaryId{7}, .roleId = DictionaryId{3}}};

    for (auto const entryCount : std::to_array<std::size_t>({0, 1, 3}))
    {
      CAPTURE(entryCount);
      auto const expected = std::span{entries}.first(entryCount);
      auto const bytes =
        performanceRecord(TrackPerformanceBlock{.recordingDate = RecordingDate{.year = 1981}}, expected);
      auto const view = TrackView{{}, bytes};
      REQUIRE(view.isColdValid());
      auto const credits = view.performance().credits();
      REQUIRE(credits.size() == entryCount);
      CHECK(utility::bytes::view(credits).data() ==
            bytes.data() + sizeof(TrackColdHeader) + sizeof(TrackPerformanceBlock));

      for (std::size_t i = 0; i < entryCount; ++i)
      {
        CHECK(credits[i].nameId == expected[i].nameId);
        CHECK(credits[i].roleId == expected[i].roleId);
      }
    }
  }

  TEST_CASE("TrackView - performance gate requires an exact prefix plus whole eight-byte entries",
            "[library][unit][track-performance]")
  {
    auto bytes = performanceRecord(TrackPerformanceBlock{.recordingDate = RecordingDate{.year = 1981}});
    auto* header = utility::layout::viewMutable<TrackColdHeader>(bytes);

    SECTION("undersized prefix")
    {
      header->uriOffset = sizeof(TrackColdHeader) + 8;
    }

    SECTION("four-byte malformed tail")
    {
      header->uriOffset = sizeof(TrackColdHeader) + sizeof(TrackPerformanceBlock) + 4;
      bytes.resize(alignToWord(header->uriOffset + header->uriLength));
    }

    checkPerformanceGateRejects(bytes);
  }

  TEST_CASE("TrackView - segment count gate widens before addition and bounds every subspan",
            "[library][unit][track-performance]")
  {
    auto const entries = std::array{TrackCreditEntry{.nameId = DictionaryId{1}},
                                    TrackCreditEntry{.nameId = DictionaryId{2}},
                                    TrackCreditEntry{.nameId = DictionaryId{3}},
                                    TrackCreditEntry{.nameId = DictionaryId{4}}};
    auto prefix = TrackPerformanceBlock{.sectionCounts = {1, 1, 1}};

    SECTION("each explicit section and inferred performer")
    {
      auto const bytes = performanceRecord(prefix, entries);
      auto const view = TrackView{{}, bytes};
      REQUIRE(view.isColdValid());
      REQUIRE(detail::TrackColdReader{bytes}.isValid());
      auto const performance = view.performance();
      REQUIRE(performance.credits().size() == 4);

      for (std::size_t index = 0; index < kCreditKindCount; ++index)
      {
        auto const section = performance.credits(static_cast<CreditKind>(index));
        REQUIRE(section.size() == 1);
        CHECK(section.data() == performance.credits().data() + index);
        CHECK(section.front().nameId == DictionaryId{static_cast<std::uint32_t>(index + 1)});
      }
    }

    SECTION("sum exceeds tail")
    {
      prefix.sectionCounts = {2, 2, 1};
      checkPerformanceGateRejects(performanceRecord(prefix, entries));
    }

    SECTION("sum would wrap in sixteen bits")
    {
      prefix.sectionCounts = {65535, 65535, 2};
      checkPerformanceGateRejects(performanceRecord(prefix, entries));
    }

    SECTION("reserved word belongs to deep gate")
    {
      prefix.reserved = 1;
      auto const bytes = performanceRecord(prefix, entries);
      CHECK(TrackView{{}, bytes}.isColdValid());
      CHECK_FALSE(detail::TrackColdReader{bytes}.isValid());
    }
  }

  TEST_CASE("TrackColdReader - performance tail rejects zero name IDs without linear work in TrackView",
            "[library][unit][track-performance]")
  {
    auto const entries = std::array{TrackCreditEntry{.nameId = DictionaryId{1}}, TrackCreditEntry{}};
    auto const bytes = performanceRecord({}, entries);
    CHECK(TrackView{{}, bytes}.isColdValid());
    CHECK_FALSE(detail::TrackColdReader{bytes}.isValid());
    auto const res = validateSerializedColdTrack(bytes);
    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::CorruptData);
  }

  TEST_CASE("TrackView - unused cold slot and obsolete mixed block are not accepted",
            "[library][unit][track-performance]")
  {
    auto bytes = performanceRecord(TrackPerformanceBlock{.recordingDate = RecordingDate{.year = 1981}});
    auto* header = utility::layout::viewMutable<TrackColdHeader>(bytes);

    SECTION("unused slot remains zero")
    {
      header->blockOffsets[4] = header->uriOffset;
    }

    SECTION("old 24-byte mixed block is not a work block")
    {
      header->blockOffsets[3] = 0;
      header->blockOffsets[1] = sizeof(TrackColdHeader);
      header->uriOffset = sizeof(TrackColdHeader) + 24;
      bytes.resize(alignToWord(header->uriOffset + header->uriLength));
    }

    checkPerformanceGateRejects(bytes);
  }

  TEST_CASE("MusicLibrary - open rejects an exact all-zero work block without rewriting either track record",
            "[library][unit][track-performance]")
  {
    auto const temp = ao::test::TempDir{};
    constexpr auto kUri = std::string_view{"track.flac"};
    constexpr std::size_t kWorkBytes = 12;
    constexpr auto kUriOffset = sizeof(TrackColdHeader) + kWorkBytes;
    auto bytes = std::vector<std::byte>(alignToWord(kUriOffset + kUri.size()), std::byte{0});
    auto header = TrackColdHeader{
      .uriOffset = static_cast<std::uint16_t>(kUriOffset), .uriLength = static_cast<std::uint16_t>(kUri.size())};
    header.blockOffsets[trackColdBlockSlotIndex(TrackColdBlockSlot::Work)] = sizeof(TrackColdHeader);
    std::memcpy(bytes.data(), &header, sizeof(header));
    std::memcpy(bytes.data() + kUriOffset, kUri.data(), kUri.size());
    REQUIRE(header.uriOffset - header.blockOffsets[trackColdBlockSlotIndex(TrackColdBlockSlot::Work)] == 12);
    REQUIRE(TrackView{{}, bytes}.isColdValid());
    CHECK_FALSE(detail::TrackColdReader{bytes}.isValid());
    auto const validationRes = validateSerializedColdTrack(bytes);
    REQUIRE_FALSE(validationRes);
    CHECK(validationRes.error().code == Error::Code::CorruptData);
    CHECK(validationRes.error().message == "Cold Track record has a non-canonical structural layout");

    auto const hotBytes = makeHotData();
    initializeLibraryStorage(temp.path());
    seedRawTrackRow(temp.path(), 1, kUri, hotBytes, bytes);
    // These observations use isolated read-only C++ adapters, never a write
    // transaction to inspect the planted records.
    checkPersistedColdBytes(temp.path(), bytes);
    requireCorruptOpen(
      temp.path(), "Track 1 failed persisted validation: Cold Track record has a non-canonical structural layout");
    checkPersistedColdBytes(temp.path(), bytes);
    auto const optHotBytes = lmdb::test::readExistingIntegerKeyRecord(temp.path(), "tracks_hot", 1);
    REQUIRE(optHotBytes);
    CHECK(*optHotBytes == hotBytes);
  }

  TEST_CASE("MusicLibrary - open rejects malformed performance storage without rewriting it",
            "[library][unit][track-performance]")
  {
    auto const temp = ao::test::TempDir{};
    auto prefix = TrackPerformanceBlock{.recordingDate = RecordingDate{.year = 1981}};
    auto entries = std::vector<TrackCreditEntry>{};
    bool hasMalformedTail = false;
    auto expected = std::string_view{"Cold Track record has a non-canonical structural layout"};

    SECTION("invalid stored calendar date")
    {
      prefix.recordingDate = RecordingDate{.year = 1900, .month = 2, .day = 29};
    }

    SECTION("empty prefix")
    {
      prefix = {};
    }

    SECTION("zero credit name ID")
    {
      entries.push_back({});
    }

    SECTION("noncanonical four-byte tail")
    {
      hasMalformedTail = true;
    }

    SECTION("unresolved conductor ID")
    {
      prefix.sectionCounts[0] = 1;
      entries.push_back({.nameId = DictionaryId{1}});
      expected = "Track record contains an unresolved credit dictionary id";
    }

    SECTION("unresolved ensemble ID")
    {
      prefix.sectionCounts[1] = 1;
      entries.push_back({.nameId = DictionaryId{1}});
      expected = "Track record contains an unresolved credit dictionary id";
    }

    SECTION("unresolved soloist ID")
    {
      prefix.sectionCounts[2] = 1;
      entries.push_back({.nameId = DictionaryId{1}});
      expected = "Track record contains an unresolved credit dictionary id";
    }

    SECTION("unresolved performer name ID")
    {
      entries.push_back({.nameId = DictionaryId{1}});
      expected = "Track record contains an unresolved credit dictionary id";
    }

    SECTION("reserved prefix word")
    {
      prefix.reserved = 1;
    }

    SECTION("classified count exceeds tail")
    {
      prefix.sectionCounts = {65535, 65535, 2};
    }

    SECTION("unresolved credit role ID")
    {
      // An admitted dictionary name isolates rejection of the missing role.
      auto library = makeTestMusicLibrary(temp.path(), temp.path());
      auto transaction = writeTransaction(library);
      auto nameRes = physicalDictionary(transaction).intern("Name");
      REQUIRE(nameRes);
      entries.push_back({.nameId = *nameRes, .roleId = DictionaryId{2}});
      REQUIRE(transaction.commit());
      expected = "Track record contains an unresolved credit dictionary id";
    }

    auto bytes = performanceRecord(prefix, entries);

    if (hasMalformedTail)
    {
      auto const uriOffset = utility::layout::view<TrackColdHeader>(bytes)->uriOffset;
      bytes.insert(bytes.begin() + uriOffset, 4, std::byte{0});
      utility::layout::viewMutable<TrackColdHeader>(bytes)->uriOffset = static_cast<std::uint16_t>(uriOffset + 4);
    }

    initializeLibraryStorage(temp.path());
    seedRawTrackRow(temp.path(), 1, "track.flac", makeHotData(), bytes);
    auto const res = openTestMusicLibrary(temp.path(), temp.path());
    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::CorruptData);
    CHECK(res.error().message == "Track 1 failed persisted validation: " + std::string{expected});
    checkPersistedColdBytes(temp.path(), bytes);
  }

  TEST_CASE("MusicLibrary - open enforces trimmed nonempty credit text on admitted dictionary references",
            "[library][unit][track-performance]")
  {
    auto const temp = ao::test::TempDir{};
    auto name = std::string_view{"Name"};
    auto role = std::string_view{"piano"};

    SECTION("empty dictionary name")
    {
      name = "";
    }

    SECTION("blank dictionary name")
    {
      name = " \t\r\n\f\v";
    }

    SECTION("untrimmed dictionary name")
    {
      name = " Name";
    }

    SECTION("empty dictionary role must use ID zero")
    {
      role = "";
    }

    SECTION("blank dictionary role must use ID zero")
    {
      role = "\t ";
    }

    SECTION("untrimmed dictionary role")
    {
      role = "piano\n";
    }

    auto entry = TrackCreditEntry{};
    {
      auto library = makeTestMusicLibrary(temp.path(), temp.path());
      auto transaction = writeTransaction(library);
      auto nameRes = physicalDictionary(transaction).intern(name);
      auto roleRes = physicalDictionary(transaction).intern(role);
      REQUIRE(nameRes);
      REQUIRE(roleRes);
      entry = {.nameId = *nameRes, .roleId = *roleRes};
      REQUIRE(transaction.commit());
    }

    auto const entries = std::array{entry};
    auto const bytes = performanceRecord({}, entries);
    REQUIRE(detail::TrackColdReader{bytes}.isValid());
    seedRawTrackRow(temp.path(), 1, "track.flac", makeHotData(), bytes);
    requireCorruptOpen(
      temp.path(), "Track 1 failed persisted validation: Track record contains non-canonical credit text");
    checkPersistedColdBytes(temp.path(), bytes);
  }
} // namespace ao::library::test
