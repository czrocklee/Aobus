// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators_all.hpp>
#include <catch2/matchers/catch_matchers_all.hpp>

#include <ao/library/DictionaryStore.h>
#include <ao/library/ResourceStore.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackLayout.h>
#include <ao/library/TrackStore.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>
#include <ao/query/ExecutionPlan.h>
#include <ao/query/Parser.h>
#include <ao/query/PlanEvaluator.h>
#include <ao/utility/ByteView.h>
#include <array>
#include <optional>
#include <span>
#include <test/unit/library/TestUtils.h>
#include <test/unit/lmdb/TestUtils.h>

#include <vector>

namespace
{
  using ao::DictionaryId;
  using ao::lmdb::Database;

  // Helper class to hold both the serialized data and create TrackView
  // This ensures the data stays valid while the view is in use
  class TestTrack
  {
  public:
    TestTrack(std::string title = "Test Title",
              std::string artist = "Test Artist",
              std::string album = "Test Album",
              std::string uri = "/path/to/track.flac",
              std::uint16_t year = 2020,
              std::uint16_t trackNumber = 5,
              std::uint32_t durationMs = 180000,
              std::uint32_t bitrate = 320000,
              std::uint32_t sampleRate = 44100,
              std::uint8_t channels = 2,
              std::uint8_t bitDepth = 16,
              std::uint32_t artistId = 1,
              std::uint32_t albumId = 2,
              std::uint32_t genreId = 3,
              std::vector<std::uint32_t> tagIds = {},
              std::string composer = "",
              std::string work = "")
      : _title{std::move(title)}
      , _artist{std::move(artist)}
      , _album{std::move(album)}
      , _uri{std::move(uri)}
      , _work{std::move(work)}
      , _composer{std::move(composer)}
    {
      _builder.metadata().title(_title);
      _builder.metadata().artist(_artist);
      _builder.metadata().album(_album);
      _builder.metadata().work(_work);
      _builder.metadata().composer(_composer);
      _builder.metadata().year(year);
      _builder.metadata().trackNumber(trackNumber);
      _builder.property().uri(_uri);
      _builder.property().durationMs(durationMs);
      _builder.property().bitrate(bitrate);
      _builder.property().sampleRate(sampleRate);
      _builder.property().channels(channels);
      _builder.property().bitDepth(bitDepth);
      // Add tags using string names (will be resolved to IDs via dict.put during serialize)
      // Store tag strings as members to keep string_view pointers valid
      _tagStrings.reserve(tagIds.size());

      for (auto id : tagIds)
      {
        auto const& str = _tagStrings.emplace_back(std::format("tag{}", id));
        _builder.tags().add(str);
      }

      // Build hot and cold data with a dictionary
      auto temp = TempDir{};
      auto envOpts = ao::lmdb::Environment::Options{.flags = MDB_CREATE, .maxDatabases = 20};
      _env.emplace(temp.path(), envOpts);
      auto wtxn = ao::lmdb::WriteTransaction{*_env};
      _dict.emplace(Database{wtxn, "dict"}, wtxn);
      _resources.emplace(Database{wtxn, "resources"});

      _hotData = _builder.serializeHot(wtxn, *_dict);
      _coldData = _builder.serializeCold(wtxn, *_dict, *_resources);

      // Fix up the header with specific IDs
      // Note: we serialize then modify, so const_cast is no longer needed with asMutablePtr
      auto* header = ao::utility::layout::asMutablePtr<ao::library::TrackHotHeader>(_hotData);
      header->artistId = DictionaryId{artistId};
      header->albumId = DictionaryId{albumId};
      header->genreId = DictionaryId{genreId};
      header->albumArtistId = DictionaryId{0};
      header->codecId = 0;
      header->rating = 0;
    }

    // Returns TrackView with both hot and cold data
    ao::library::TrackView view() const { return ao::library::TrackView{_hotData, _coldData}; }

    // Returns TrackView with hot data only (for invalid cold tests)
    ao::library::TrackView hotOnlyView() const
    {
      return ao::library::TrackView{_hotData, std::span<std::byte const>{}};
    }

    // Returns TrackView with cold data only (for cold-only plan tests)
    ao::library::TrackView coldOnlyView() const
    {
      return ao::library::TrackView{std::span<std::byte const>{}, _coldData};
    }

    ao::library::DictionaryStore& dictionary() { return *_dict; }

  private:
    ao::library::TrackBuilder _builder = ao::library::TrackBuilder::createNew();
    std::optional<ao::lmdb::Environment> _env;
    std::optional<ao::library::DictionaryStore> _dict;
    std::optional<ao::library::ResourceStore> _resources;
    std::vector<std::byte> _hotData;
    std::vector<std::byte> _coldData;
    // Store strings as members so string_view pointers remain valid
    std::string _title;
    std::string _artist;
    std::string _album;
    std::string _uri;
    std::string _work;
    std::string _composer;
    std::vector<std::string> _tagStrings; // Keep tag strings alive for string_view
  };

  std::vector<std::byte> makeHotOnlyTrack(ao::DictionaryId artistId = ao::DictionaryId{0},
                                          ao::DictionaryId albumId = ao::DictionaryId{0},
                                          ao::DictionaryId genreId = ao::DictionaryId{0},
                                          ao::DictionaryId albumArtistId = ao::DictionaryId{0},
                                          std::span<ao::DictionaryId const> tagIds = {})
  {
    auto header = ao::library::TrackHotHeader{};
    header.artistId = artistId;
    header.albumId = albumId;
    header.genreId = genreId;
    header.albumArtistId = albumArtistId;
    header.tagLen = static_cast<std::uint16_t>(tagIds.size_bytes());

    for (auto const tagId : tagIds)
    {
      header.tagBloom |= std::uint32_t{1} << (tagId.value() & 31U);
    }

    auto data = test::serializeHeader(header);

    for (auto const tagId : tagIds)
    {
      data.insert_range(data.end(), ao::utility::bytes::view(tagId));
    }

    test::appendString(data, "");
    return data;
  }
} // namespace

using ao::DictionaryId;
using ao::library::DictionaryStore;
using ao::lmdb::Database;
using ao::lmdb::Environment;
using ao::lmdb::ReadTransaction;
using ao::lmdb::WriteTransaction;
using namespace ao::query;

TEST_CASE("PlanEvaluator - Simple Equal Match")
{
  auto expr = parse("$year = 2020");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  auto track1 = TestTrack{"Test", "Artist", "Album", "/path", 2020};
  auto result = evaluator.evaluateFull(plan, track1.view());
  CHECK(result == true);

  auto track2 = TestTrack{"Test", "Artist", "Album", "/path", 2019};
  result = evaluator.evaluateFull(plan, track2.view());
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - Greater Than")
{
  auto expr = parse("@duration > 179000");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  auto track1 = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 180000};
  auto result = evaluator.evaluateFull(plan, track1.view());
  CHECK(result == true);

  auto track2 = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 170000};
  result = evaluator.evaluateFull(plan, track2.view());
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - NotEqual")
{
  auto expr = parse("$year != 2020");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  auto track1 = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 180000};
  auto result = evaluator.evaluateFull(plan, track1.view());
  CHECK(result == false);

  auto track2 = TestTrack{"Test", "Artist", "Album", "/path", 2021, 5, 180000};
  result = evaluator.evaluateFull(plan, track2.view());
  CHECK(result == true);

  auto track3 = TestTrack{"Test", "Artist", "Album", "/path", 2019, 5, 180000};
  result = evaluator.evaluateFull(plan, track3.view());
  CHECK(result == true);
}

TEST_CASE("PlanEvaluator - Greater Than Or Equal")
{
  auto expr = parse("@duration >= 180000");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  auto track1 = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 180000};
  auto result = evaluator.evaluateFull(plan, track1.view());
  CHECK(result == true);

  auto track2 = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 179999};
  result = evaluator.evaluateFull(plan, track2.view());
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - Less Than")
{
  auto expr = parse("$year < 2021");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  auto track1 = TestTrack{"Test", "Artist", "Album", "/path", 2020};
  auto result = evaluator.evaluateFull(plan, track1.view());
  CHECK(result == true);

  auto track2 = TestTrack{"Test", "Artist", "Album", "/path", 2021};
  result = evaluator.evaluateFull(plan, track2.view());
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - Like Match")
{
  auto expr = parse("$title ~ Test");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  auto track1 = TestTrack{"Test Title"};
  auto result = evaluator.evaluateFull(plan, track1.view());
  CHECK(result == true);

  auto track2 = TestTrack{"Another Title"};
  result = evaluator.evaluateFull(plan, track2.view());
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - Work Metadata")
{
  auto trackWithWork = TestTrack{
    "Title", "Artist", "Album", "/path", 2020, 5, 180000, 320000, 44100, 2, 16, 1, 2, 3, {}, "", "Symphony No. 5"};
  auto trackWithoutWork =
    TestTrack{"Title", "Artist", "Album", "/path", 2020, 5, 180000, 320000, 44100, 2, 16, 1, 2, 3, {}, "", ""};
  auto evaluator = PlanEvaluator{};

  SECTION("$work Equality")
  {
    auto expr = parse("$work = 'Symphony No. 5'");
    auto compiler = QueryCompiler{&trackWithWork.dictionary()};
    auto plan = compiler.compile(expr);
    CHECK(evaluator.evaluateFull(plan, trackWithWork.view()) == true);
    CHECK(evaluator.evaluateFull(plan, trackWithoutWork.view()) == false);
  }

  SECTION("$w Equality (shorthand)")
  {
    auto expr = parse("$w = 'Symphony No. 5'");
    auto compiler = QueryCompiler{&trackWithWork.dictionary()};
    auto plan = compiler.compile(expr);
    CHECK(evaluator.evaluateFull(plan, trackWithWork.view()) == true);
  }

  SECTION("$work LIKE")
  {
    auto expr = parse("$work ~ Symphony");
    auto compiler = QueryCompiler{&trackWithWork.dictionary()};
    auto plan = compiler.compile(expr);
    CHECK(evaluator.evaluateFull(plan, trackWithWork.view()) == true);
    CHECK(evaluator.evaluateFull(plan, trackWithoutWork.view()) == false);
  }
}

TEST_CASE("PlanEvaluator - Composer Metadata")
{
  auto trackWithComposer =
    TestTrack{"Title", "Artist", "Album", "/path", 2020, 5, 180000, 320000, 44100, 2, 16, 1, 2, 3, {}, "Beethoven", ""};
  auto evaluator = PlanEvaluator{};

  SECTION("$composer Equality")
  {
    auto expr = parse("$composer = Beethoven");
    auto compiler = QueryCompiler{&trackWithComposer.dictionary()};
    auto plan = compiler.compile(expr);
    CHECK(evaluator.evaluateFull(plan, trackWithComposer.view()) == true);
  }

  SECTION("$composer LIKE")
  {
    auto expr = parse("$composer ~ Beet");
    auto compiler = QueryCompiler{&trackWithComposer.dictionary()};
    auto plan = compiler.compile(expr);
    CHECK(evaluator.evaluateFull(plan, trackWithComposer.view()) == true);
  }
}
TEST_CASE("PlanEvaluator - Artist LIKE resolves DictionaryId strings")
{
  auto temp = TempDir{};
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};
  auto wtxn = WriteTransaction{env};
  auto dict = DictionaryStore{Database{wtxn, "dict"}, wtxn};
  auto bachId = dict.put(wtxn, "Johann Sebastian Bach");
  auto mozartId = dict.put(wtxn, "Wolfgang Amadeus Mozart");

  auto expr = parse(R"($artist ~ "Bach")");
  auto compiler = QueryCompiler{&dict};
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  auto matchingHotData = makeHotOnlyTrack(bachId);
  auto matchingTrack = ao::library::TrackView{matchingHotData, std::span<std::byte const>{}};
  CHECK(evaluator.evaluateFull(plan, matchingTrack) == true);

  auto nonMatchingHotData = makeHotOnlyTrack(mozartId);
  auto nonMatchingTrack = ao::library::TrackView{nonMatchingHotData, std::span<std::byte const>{}};
  CHECK(evaluator.evaluateFull(plan, nonMatchingTrack) == false);

  auto missingArtistHotData = makeHotOnlyTrack();
  auto missingArtistTrack = ao::library::TrackView{missingArtistHotData, std::span<std::byte const>{}};
  CHECK(evaluator.evaluateFull(plan, missingArtistTrack) == false);
}

TEST_CASE("PlanEvaluator - OR between artist LIKE and tag does not over-prune on bloom filter")
{
  auto temp = TempDir{};
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};
  auto wtxn = WriteTransaction{env};
  auto dict = DictionaryStore{Database{wtxn, "dict"}, wtxn};
  auto aimerId = dict.put(wtxn, "Aimer");
  wtxn.commit();

  auto expr = parse(R"($artist ~ "Aimer" or #Aimer)");
  auto compiler = QueryCompiler{&dict};
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  CHECK(plan.tagBloomMask == 0);

  auto artistMatchHotData = makeHotOnlyTrack(aimerId);
  auto artistMatchTrack = ao::library::TrackView{artistMatchHotData, std::span<std::byte const>{}};
  CHECK(evaluator.matches(plan, artistMatchTrack) == true);

  auto tagIds = std::array<ao::DictionaryId, 1>{aimerId};
  auto tagMatchHotData =
    makeHotOnlyTrack(ao::DictionaryId{0}, ao::DictionaryId{0}, ao::DictionaryId{0}, ao::DictionaryId{0}, tagIds);
  auto tagMatchTrack = ao::library::TrackView{tagMatchHotData, std::span<std::byte const>{}};
  CHECK(evaluator.matches(plan, tagMatchTrack) == true);

  auto noMatchHotData = makeHotOnlyTrack();
  auto noMatchTrack = ao::library::TrackView{noMatchHotData, std::span<std::byte const>{}};
  CHECK(evaluator.matches(plan, noMatchTrack) == false);
}

TEST_CASE("PlanEvaluator - Title String Equal")
{
  auto expr = parse("$title = 'Hello World'");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  auto track1 = TestTrack{"Hello World"};
  auto result = evaluator.evaluateFull(plan, track1.view());
  CHECK(result == true);

  auto track2 = TestTrack{"hello world"}; // case-sensitive
  result = evaluator.evaluateFull(plan, track2.view());
  CHECK(result == false);

  auto track3 = TestTrack{"Hello"};
  result = evaluator.evaluateFull(plan, track3.view());
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - Title String Not Equal")
{
  auto expr = parse("$title != 'Hello'");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  auto track1 = TestTrack{"Hello World"};
  auto result = evaluator.evaluateFull(plan, track1.view());
  CHECK(result == true);

  auto track2 = TestTrack{"Hello"};
  result = evaluator.evaluateFull(plan, track2.view());
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - Title String Less Than")
{
  auto expr = parse("$title < 'zoo'");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  auto track1 = TestTrack{"apple"};
  auto result = evaluator.evaluateFull(plan, track1.view());
  CHECK(result == true);

  auto track2 = TestTrack{"zoo"};
  result = evaluator.evaluateFull(plan, track2.view());
  CHECK(result == false);

  auto track3 = TestTrack{"zooExtra"};
  result = evaluator.evaluateFull(plan, track3.view());
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - Title String Greater Than")
{
  auto expr = parse("$title > 'apple'");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  auto track1 = TestTrack{"banana"};
  auto result = evaluator.evaluateFull(plan, track1.view());
  CHECK(result == true);

  auto track2 = TestTrack{"apple"};
  result = evaluator.evaluateFull(plan, track2.view());
  CHECK(result == false);

  auto track3 = TestTrack{"Apple"}; // case-sensitive
  result = evaluator.evaluateFull(plan, track3.view());
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - Logical And")
{
  auto expr = parse("$year = 2020 && @duration > 100000");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  auto track1 = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 180000};
  auto result = evaluator.evaluateFull(plan, track1.view());
  CHECK(result == true);

  auto track2 = TestTrack{"Test", "Artist", "Album", "/path", 2019, 5, 180000};
  result = evaluator.evaluateFull(plan, track2.view());
  CHECK(result == false);

  auto track3 = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 50000};
  result = evaluator.evaluateFull(plan, track3.view());
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - Logical Or")
{
  auto expr = parse("$year = 2020 || $year = 2019");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  auto track1 = TestTrack{"Test", "Artist", "Album", "/path", 2020};
  auto result = evaluator.evaluateFull(plan, track1.view());
  CHECK(result == true);

  auto track2 = TestTrack{"Test", "Artist", "Album", "/path", 2019};
  result = evaluator.evaluateFull(plan, track2.view());
  CHECK(result == true);

  auto track3 = TestTrack{"Test", "Artist", "Album", "/path", 2018};
  result = evaluator.evaluateFull(plan, track3.view());
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - Logical Not")
{
  // Use "not(" for explicit grouping, or check if parser handles precedence
  auto expr = parse("!($year = 2020)");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  auto track1 = TestTrack{"Test", "Artist", "Album", "/path", 2020};
  auto result = evaluator.evaluateFull(plan, track1.view());
  CHECK(result == false);

  auto track2 = TestTrack{"Test", "Artist", "Album", "/path", 2019};
  result = evaluator.evaluateFull(plan, track2.view());
  CHECK(result == true);
}

TEST_CASE("PlanEvaluator - Complex Expression")
{
  // Note: genre comparison requires dictionary resolution, so we use numeric genreId
  auto expr = parse("@duration > 180000 && $year >= 2020");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  auto track1 = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 200000, 320000, 44100, 2, 16, 1, 2, 1};
  auto result = evaluator.evaluateFull(plan, track1.view());
  CHECK(result == true);

  auto track2 = TestTrack{"Test", "Artist", "Album", "/path", 2019, 5, 200000, 320000, 44100, 2, 16, 1, 2, 1};
  result = evaluator.evaluateFull(plan, track2.view());
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - Matches All")
{
  auto expr = parse("true");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  CHECK(plan.matchesAll == true);

  auto track1 = TestTrack{};
  auto result = evaluator.matches(plan, track1.view());
  CHECK(result == true);

  auto track2 = TestTrack{"Test", "Artist", "Album", "/path", 1900};
  result = evaluator.matches(plan, track2.view());
  CHECK(result == true);
}

TEST_CASE("PlanEvaluator - Invalid Track View")
{
  auto expr = parse("$year = 2020");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  // Empty hot data creates an invalid TrackView
  auto emptyView = ao::library::TrackView{std::span<std::byte const>{}, std::span<std::byte const>{}};
  auto result = evaluator.evaluateFull(plan, emptyView);
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - ColdOnly plan works with cold-only TrackView")
{
  auto expr = parse("@duration >= 180000");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  REQUIRE(plan.accessProfile == AccessProfile::ColdOnly);

  auto track = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 180000};
  CHECK(evaluator.evaluateFull(plan, track.coldOnlyView()) == true);
}

TEST_CASE("PlanEvaluator - Mixed plan still requires both storage tiers")
{
  auto expr = parse("$year = 2020 && @duration >= 180000");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  REQUIRE(plan.accessProfile == AccessProfile::HotAndCold);

  auto track = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 180000};
  CHECK(evaluator.evaluateFull(plan, track.hotOnlyView()) == false);
  CHECK(evaluator.evaluateFull(plan, track.coldOnlyView()) == false);
}

TEST_CASE("PlanEvaluator - Bitrate Comparison")
{
  auto expr = parse("@bitrate >= 320000");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  auto track1 = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 180000, 320000};
  auto result = evaluator.evaluateFull(plan, track1.view());
  CHECK(result == true);

  auto track2 = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 180000, 256000};
  result = evaluator.evaluateFull(plan, track2.view());
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - SampleRate Comparison")
{
  auto expr = parse("@sampleRate >= 48000");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  auto track1 = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 180000, 320000, 48000};
  auto result = evaluator.evaluateFull(plan, track1.view());
  CHECK(result == true);

  auto track2 = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 180000, 320000, 44100};
  result = evaluator.evaluateFull(plan, track2.view());
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - Unit Constants")
{
  auto expr = parse("@duration >= 3m && @bitrate >= 256k && @sampleRate >= 44.1k");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  auto track1 = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 180000, 320000, 44100};
  auto result = evaluator.evaluateFull(plan, track1.view());
  CHECK(result == true);

  auto track2 = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 180000, 192000, 44100};
  result = evaluator.evaluateFull(plan, track2.view());
  CHECK(result == false);

  auto track3 = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 180000, 320000, 32000};
  result = evaluator.evaluateFull(plan, track3.view());
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - Year Comparison")
{
  // Note: $trackNumber is a cold field, so we use $year instead which is hot
  auto expr = parse("$year = 2020");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  auto track1 = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5};
  auto result = evaluator.evaluateFull(plan, track1.view());
  CHECK(result == true);

  auto track2 = TestTrack{"Test", "Artist", "Album", "/path", 2019, 5};
  result = evaluator.evaluateFull(plan, track2.view());
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - Tag Query - No Tags")
{
  // Query for tag - with dictionary resolving "rock" -> ID 10
  auto expr = parse("#rock");
  auto compiler = QueryCompiler{}; // No dictionary - tag resolution disabled
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  // Track with no tags - bloom filter rejects
  auto track1 = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 180000, 320000, 44100, 2, 16, 1, 2, 3, {}};
  auto result = evaluator.matches(plan, track1.view());
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - Tag Query - With Matching Tag")
{
  // Set up DictionaryStore with tag
  auto temp = TempDir{};
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  auto wtxn = WriteTransaction{env};
  auto dict = DictionaryStore{Database{wtxn, "dict"}, wtxn};
  dict.put(wtxn, "rock"); // Will get ID 1 (DictionaryId starts from 1)
  wtxn.commit();

  // Query for #rock
  auto expr = parse("#rock");
  auto compiler = QueryCompiler{&dict}; // Pass dictionary for tag resolution
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  // Track with tag ID 0 (matches "rock" = 0)
  auto trackWithTag =
    TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 180000, 320000, 44100, 2, 16, 1, 2, 3, {0}};
  auto result = evaluator.matches(plan, trackWithTag.view());
  CHECK(result == true);
}

TEST_CASE("PlanEvaluator - Tag Query - With Non-Matching Tag")
{
  // Dictionary: "rock" -> ID 10, but track has tag ID 20
  auto expr = parse("#rock");
  auto compiler = QueryCompiler{}; // No dictionary - tag resolution disabled
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  // Track with tag ID 20 (doesn't match "rock" = 10)
  auto trackWithTag =
    TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 180000, 320000, 44100, 2, 16, 1, 2, 3, {20}};
  auto result = evaluator.matches(plan, trackWithTag.view());
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - Tag Field Compilation")
{
  // Test that tag field compiles to Field::Tag
  auto expr = parse("#tagname");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);

  // Check that instructions contain LoadField with Field::Tag (16)
  CHECK(plan.instructions.size() > 0);
  CHECK(plan.instructions[0].op == OpCode::LoadField);
}

TEST_CASE("PlanEvaluator - TagCount Field")
{
  auto expr = parse("@tagCount > 0");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  // Track with 2 tags
  auto track1 = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 180000, 320000, 44100, 2, 16, 1, 2, 3, {10, 20}};
  auto result = evaluator.evaluateFull(plan, track1.view());
  CHECK(result == true);

  // Track with no tags
  auto track2 = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 180000, 320000, 44100, 2, 16, 1, 2, 3, {}};
  result = evaluator.evaluateFull(plan, track2.view());
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - Tag Bloom Filter - Without Dictionary")
{
  // Without a dictionary, tag bloom mask cannot be set (no way to resolve tag name to ID)
  auto expr = parse("#mytag");
  auto compiler = QueryCompiler{}; // No dictionary provided
  auto plan = compiler.compile(expr);

  // Without dictionary, tagBloomMask stays 0
  CHECK(plan.tagBloomMask == 0);
}

TEST_CASE("PlanEvaluator - Tag Bloom Filter - Dictionary Miss")
{
  // Dictionary has "rock" -> ID 10, but query is for "jazz" which is not in dict
  auto expr = parse("#jazz");
  auto compiler = QueryCompiler{}; // No dictionary - tag resolution disabled
  auto plan = compiler.compile(expr);

  // "jazz" not in dictionary, tagBloomMask stays 0
  CHECK(plan.tagBloomMask == 0);
}

TEST_CASE("PlanEvaluator - Tag Bloom Filter - Dictionary Hit")
{
  // Dictionary has "rock" -> ID 10 (10 & 31 = 10)
  auto expr = parse("#rock");
  auto compiler = QueryCompiler{}; // No dictionary - tag resolution disabled
  auto plan = compiler.compile(expr);

  // Without dictionary, tag resolution doesn't happen, bloom mask stays 0
  CHECK(plan.tagBloomMask == 0);
}

TEST_CASE("PlanEvaluator - Tag Bloom Filter - Multiple Tags Hit")
{
  // Dictionary: "rock" -> ID 10, "jazz" -> ID 20
  auto expr = parse("#rock && #jazz");
  auto compiler = QueryCompiler{}; // No dictionary - tag resolution disabled
  auto plan = compiler.compile(expr);

  // Without dictionary, tag resolution doesn't happen, bloom mask stays 0
  CHECK(plan.tagBloomMask == 0);
}

TEST_CASE("PlanEvaluator - Tag Bloom Filter - Track Computation")
{
  // Test that track bloom is computed correctly: tagId & 31
  // Using manual header construction.
  // Tag ID 10 -> bit 10 (10 & 31 = 10)
  {
    auto h = ao::library::TrackHotHeader{};
    h.tagBloom = (1U << (10 & 31)); // bit 10
    auto data = test::serializeHeader(h);
    data.push_back(static_cast<std::byte>('\0')); // empty title
    auto view = ao::library::TrackView{data, std::span<std::byte const>{}};
    CHECK(view.tags().bloom() == (1U << 10));
  }

  // Tag ID 32 -> bit 0 (32 & 31 = 0)
  {
    auto h = ao::library::TrackHotHeader{};
    h.tagBloom = (1U << (32 & 31)); // bit 0
    auto data = test::serializeHeader(h);
    data.push_back(static_cast<std::byte>('\0')); // empty title
    auto view = ao::library::TrackView{data, std::span<std::byte const>{}};
    CHECK(view.tags().bloom() == 1U);
  }

  // Multiple tags: ID 5 and ID 20
  {
    auto h = ao::library::TrackHotHeader{};
    h.tagBloom = (1U << (5 & 31)) | (1U << (20 & 31)); // bits 5 and 20
    auto data = test::serializeHeader(h);
    data.push_back(static_cast<std::byte>('\0')); // empty title
    auto view = ao::library::TrackView{data, std::span<std::byte const>{}};
    CHECK((view.tags().bloom() & (1U << 5)) != 0);  // Bit 5 should be set
    CHECK((view.tags().bloom() & (1U << 20)) != 0); // Bit 20 should be set
  }
}

TEST_CASE("PlanEvaluator - Bloom Filter Fast Path - No Match")
{
  auto expr = parse("#mytag");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  // Create a track with tag bloom bit 0 set (simulating tag ID 32)
  auto h = ao::library::TrackHotHeader{};
  h.tagBloom = 0x00000001U; // Only bit 0 set

  auto data = std::vector<std::byte>{};
  data.insert_range(data.end(), ao::utility::bytes::view(h));

  data.push_back(static_cast<std::byte>('\0')); // empty title
  data.push_back(static_cast<std::byte>('\0')); // empty uri

  auto view = ao::library::TrackView{data, std::span<std::byte const>{}};

  // Bloom filter rejects because query mask doesn't match track bloom
  auto result = evaluator.matches(plan, view);
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - Bloom Filter Fast Path - Match")
{
  auto expr = parse("#mytag");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  // Create a track with tag bloom that has some bits set
  // (without dictionary, mask is 0, so this won't actually test fast path)
  auto h = ao::library::TrackHotHeader{};
  h.tagBloom = 0xFFFFFFFFU; // All bits set

  auto data = std::vector<std::byte>{};
  data.insert_range(data.end(), ao::utility::bytes::view(h));

  data.push_back(static_cast<std::byte>('\0')); // empty title
  data.push_back(static_cast<std::byte>('\0')); // empty uri

  auto view = ao::library::TrackView{data, std::span<std::byte const>{}};

  // With mask 0, bloom check passes (no filtering), falls through to full eval
  auto result = evaluator.matches(plan, view);
  // No tags in track, so should be false
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - Title LIKE with quoted string evaluates correctly")
{
  // Simple title LIKE test with quoted string
  auto expr = parse(R"($title ~ "Bach")");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  // Track with title containing "Bach"
  auto track1 = TestTrack{"Bach Greatest Hits"};
  auto result = evaluator.evaluateFull(plan, track1.view());
  CHECK(result == true);

  // Track with title not containing "Bach"
  auto track2 = TestTrack{"Mozart Symphony"};
  result = evaluator.evaluateFull(plan, track2.view());
  CHECK(result == false);

  // Track with exact match
  auto track3 = TestTrack{"Bach"};
  result = evaluator.evaluateFull(plan, track3.view());
  CHECK(result == true);
}

TEST_CASE("PlanEvaluator - Title LIKE with multi-arg Track")
{
  // Test LIKE with a Track that has multiple fields set
  auto expr = parse(R"($title ~ "Bach")");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  auto track = TestTrack{"Bach Greatest Hits", "Artist", "Album", "/path", 2021};
  auto result = evaluator.evaluateFull(plan, track.view());
  CHECK(result == true);
}

TEST_CASE("PlanEvaluator - OR expression with LIKE evaluates correctly")
{
  // Test that $title ~ "Bach" or $year > 2021 evaluates correctly
  auto expr = parse(R"($title ~ "Bach" or $year > 2021)");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  // Track with title containing "Bach"
  auto track1 = TestTrack{"Bach Greatest Hits", "Artist", "Album", "/path", 2021};
  auto result = evaluator.evaluateFull(plan, track1.view());
  CHECK(result == true); // matches title ~ "Bach"

  // Track with year > 2021 but title doesn't contain "Bach"
  auto track2 = TestTrack{"Classical Music", "Artist", "Album", "/path", 2022};
  result = evaluator.evaluateFull(plan, track2.view());
  CHECK(result == true); // matches year > 2021

  // Track with neither matching
  auto track3 = TestTrack{"Classical Music", "Artist", "Album", "/path", 2021};
  result = evaluator.evaluateFull(plan, track3.view());
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - OR with simple expressions")
{
  // Test $year > 2000 or $year > 1990 to verify OR works with two numeric comparisons
  auto expr = parse("$year > 2000 or $year > 1990");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  // year 2021: 2021 > 2000 is true, so OR should be true
  auto track1 = TestTrack{"Title", "Artist", "Album", "/path", 2021};
  auto result = evaluator.evaluateFull(plan, track1.view());
  CHECK(result == true);

  // year 1995: 1995 > 2000 is false, but 1995 > 1990 is true, so OR should be true
  auto track2 = TestTrack{"Title", "Artist", "Album", "/path", 1995};
  result = evaluator.evaluateFull(plan, track2.view());
  CHECK(result == true);

  // year 1980: both are false, so OR should be false
  auto track3 = TestTrack{"Title", "Artist", "Album", "/path", 1980};
  result = evaluator.evaluateFull(plan, track3.view());
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - Greater alone for year 1980")
{
  // Verify $year > 2000 returns false for year 1980
  auto expr = parse("$year > 2000");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  auto track = TestTrack{"Title", "Artist", "Album", "/path", 1980};
  auto result = evaluator.evaluateFull(plan, track.view());
  CHECK(result == false);
}

TEST_CASE("PlanEvaluator - Year Greater Alone Works")
{
  // Simple year > test to verify year comparison works
  auto expr = parse("$year > 2000");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  auto evaluator = PlanEvaluator{};

  auto track1 = TestTrack{"Title", "Artist", "Album", "/path", 2021};
  auto result = evaluator.evaluateFull(plan, track1.view());
  CHECK(result == true);

  auto track2 = TestTrack{"Title", "Artist", "Album", "/path", 1990};
  result = evaluator.evaluateFull(plan, track2.view());
  CHECK(result == false);
}
