// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/library/TrackRecordValidation.h"
#include "test/TestFixtureSupport.h"
#include "test/library/MusicLibraryTestSupport.h"
#include "test/library/TrackStoreTestSupport.h"
#include "test/library/WritableLibraryTestSupport.h"
#include "test/lmdb/LmdbTestSupport.h"
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
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <algorithm>
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

  TEST_CASE("TrackView - performance gate rejects invalid recording dates before any date accessor")
  {
    // Date formatting and comparison require a valid date, so the constant-time
    // read gate owns date validity rather than leaving it to deep validation.
    for (auto const date : std::array{RecordingDate{.month = 1},
                                      RecordingDate{.day = 1},
                                      RecordingDate{.year = 10000},
                                      RecordingDate{.year = 1981, .month = 13},
                                      RecordingDate{.year = 1981, .day = 1},
                                      RecordingDate{.year = 1900, .month = 2, .day = 29},
                                      RecordingDate{.year = 2000, .month = 4, .day = 31}})
    {
      CAPTURE(date.year, date.month, date.day);
      checkPerformanceGateRejects(performanceRecord(TrackPerformanceBlock{.recordingDate = date}));
    }
  }

  TEST_CASE("TrackView - performance gate leaves empty-block semantics to deep validation")
  {
    auto const bytes = performanceRecord(TrackPerformanceBlock{});
    auto const view = TrackView{{}, bytes};
    REQUIRE(view.isColdValid());
    auto const performance = view.performance();
    CHECK_FALSE(performance.empty());
    CHECK_FALSE(performance.recordingDate().isPresent());
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

  TEST_CASE("TrackView - performance gate borrows complete ordered credit tails")
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

  TEST_CASE("TrackView - performance gate requires an exact prefix plus whole twelve-byte entries")
  {
    auto bytes = performanceRecord(TrackPerformanceBlock{.recordingDate = RecordingDate{.year = 1981}});
    auto* header = utility::layout::viewMutable<TrackColdHeader>(bytes);
    auto const tailBytes = GENERATE(std::size_t{4}, std::size_t{8}, std::size_t{16});
    CAPTURE(tailBytes);
    header->uriOffset = static_cast<std::uint16_t>(sizeof(TrackColdHeader) + sizeof(TrackPerformanceBlock) + tailBytes);
    bytes.resize(alignToWord(header->uriOffset + header->uriLength));
    checkPerformanceGateRejects(bytes);
  }

  TEST_CASE("TrackView - each kind's credits are one bounded run of the entries")
  {
    auto entries = std::vector<TrackCreditEntry>{};

    for (std::size_t index = 0; index < kCreditKindCount; ++index)
    {
      entries.push_back(
        {.nameId = DictionaryId{static_cast<std::uint32_t>(index + 1)}, .kind = static_cast<std::uint8_t>(index)});
    }

    SECTION("one entry of every kind")
    {
      auto const bytes = performanceRecord({}, entries);
      auto const view = TrackView{{}, bytes};
      REQUIRE(view.isColdValid());
      REQUIRE(detail::TrackColdReader{bytes}.isValid());
      auto const performance = view.performance();
      REQUIRE(performance.credits().size() == kCreditKindCount);

      for (std::size_t index = 0; index < kCreditKindCount; ++index)
      {
        auto const section = performance.credits(static_cast<CreditKind>(index));
        REQUIRE(section.size() == 1);
        CHECK(section.data() == performance.credits().data() + index);
        CHECK(section.front().nameId == DictionaryId{static_cast<std::uint32_t>(index + 1)});
      }
    }

    SECTION("a kind with no entries is an empty run")
    {
      entries.erase(entries.begin() + 1);
      auto const bytes = performanceRecord({}, entries);
      auto const performance = TrackView{{}, bytes}.performance();
      CHECK(performance.credits(CreditKind::Ensemble).empty());
      REQUIRE(performance.credits(CreditKind::Soloist).size() == 1);
      CHECK(performance.credits(CreditKind::Soloist).front().nameId == DictionaryId{3});
    }

    // The read gate stays constant-time, so entry order, kind range, and
    // reserved bytes belong to the deep gate; a per-kind read of such entries
    // stays inside them.
    SECTION("kinds out of order")
    {
      std::ranges::reverse(entries);
      auto const bytes = performanceRecord({}, entries);
      auto const view = TrackView{{}, bytes};
      CHECK(view.isColdValid());
      CHECK_FALSE(detail::TrackColdReader{bytes}.isValid());

      for (std::size_t index = 0; index < kCreditKindCount; ++index)
      {
        auto const section = view.performance().credits(static_cast<CreditKind>(index));
        REQUIRE(section.size() == 1);
        CHECK(section.front().nameId == DictionaryId{static_cast<std::uint32_t>(index + 1)});
      }
    }

    SECTION("a kind beyond the vocabulary")
    {
      entries.back().kind = static_cast<std::uint8_t>(kCreditKindCount);
      auto const bytes = performanceRecord({}, entries);
      CHECK(TrackView{{}, bytes}.isColdValid());
      CHECK_FALSE(detail::TrackColdReader{bytes}.isValid());
    }

    SECTION("a nonzero reserved byte")
    {
      auto const reservedIndex = GENERATE(std::size_t{0}, std::size_t{1}, std::size_t{2});
      CAPTURE(reservedIndex);
      entries.front().reserved[reservedIndex] = 1;
      auto const bytes = performanceRecord({}, entries);
      CHECK(TrackView{{}, bytes}.isColdValid());
      CHECK_FALSE(detail::TrackColdReader{bytes}.isValid());
    }
  }

  TEST_CASE("TrackColdReader - performance tail rejects zero name IDs without linear work in TrackView")
  {
    auto const entries = std::array{TrackCreditEntry{.nameId = DictionaryId{1}}, TrackCreditEntry{}};
    auto const bytes = performanceRecord({}, entries);
    CHECK(TrackView{{}, bytes}.isColdValid());
    CHECK_FALSE(detail::TrackColdReader{bytes}.isValid());
    auto const res = validateSerializedColdTrack(bytes);
    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::CorruptData);
  }

  TEST_CASE("TrackView - unused cold slot and obsolete mixed block are not accepted")
  {
    auto bytes = performanceRecord(TrackPerformanceBlock{.recordingDate = RecordingDate{.year = 1981}});
    auto* header = utility::layout::viewMutable<TrackColdHeader>(bytes);

    SECTION("segment slot cannot point at URI")
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

  TEST_CASE("MusicLibrary - open rejects an exact all-zero work block without rewriting either track record")
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

  TEST_CASE("MusicLibrary - open rejects malformed performance storage without rewriting it")
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

    SECTION("unresolved credit name ID of each kind")
    {
      auto const kindIndex = GENERATE(range(std::size_t{0}, kCreditKindCount));
      CAPTURE(kindIndex);
      entries.push_back({.nameId = DictionaryId{1}, .kind = static_cast<std::uint8_t>(kindIndex)});
      expected = "Track record contains an unresolved credit dictionary id";
    }

    SECTION("nonzero reserved credit byte")
    {
      entries.push_back({.nameId = DictionaryId{1}, .reserved = {0, 1, 0}});
    }

    SECTION("credit kind beyond the vocabulary")
    {
      entries.push_back({.nameId = DictionaryId{1}, .kind = static_cast<std::uint8_t>(kCreditKindCount)});
    }

    SECTION("credit kinds out of order")
    {
      entries.push_back({.nameId = DictionaryId{1}, .kind = static_cast<std::uint8_t>(CreditKind::Performer)});
      entries.push_back({.nameId = DictionaryId{1}, .kind = static_cast<std::uint8_t>(CreditKind::Conductor)});
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

  TEST_CASE("MusicLibrary - open enforces trimmed nonempty credit text on admitted dictionary references")
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
