// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/TestUtils.h"
#include "test/unit/library/TestUtils.h"
#include "test/unit/lmdb/TestUtils.h"
#include <ao/AudioCodec.h>
#include <ao/Type.h>
#include <ao/library/CoverArt.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/ResourceStore.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackLayout.h>
#include <ao/library/TrackView.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>
#include <ao/query/Expression.h>
#include <ao/query/Field.h>
#include <ao/query/Parser.h>
#include <ao/query/PlanEvaluator.h>
#include <ao/query/QueryCompiler.h>
#include <ao/query/detail/Bytecode.h>
#include <ao/utility/ByteView.h>

#include <catch2/catch_test_macros.hpp>
#include <lmdb.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::query::test
{
  namespace
  {
    using namespace ao::library;
    using namespace ao::library::test;
    using namespace ao::lmdb;
    using namespace ao::lmdb::test;

    Expression parseOk(std::string_view text)
    {
      auto result = ::ao::query::parse(text);
      REQUIRE(result.has_value());
      return std::move(*result);
    }

    ExecutionPlan compileOk(QueryCompiler& compiler, Expression const& expr)
    {
      auto result = compiler.compile(expr);
      REQUIRE(result.has_value());
      return std::move(*result);
    }

    ExecutionPlan compileOk(QueryCompiler&& compiler, Expression const& expr)
    {
      auto local = std::move(compiler);
      return compileOk(local, expr);
    }

    struct TrackSpec final
    {
      std::string title = "Test Title";
      std::string artist = "Test Artist";
      std::string album = "Test Album";
      std::string albumArtist = {};
      std::string composer = {};
      std::string work = {};
      std::string genre = {};
      std::string uri = "/path/to/track.flac";
      std::uint16_t year = 2020;
      std::uint16_t trackNumber = 1;
      std::uint16_t trackTotal = 0;
      std::uint16_t discNumber = 0;
      std::uint16_t discTotal = 0;
      std::chrono::milliseconds duration = std::chrono::seconds{180};
      std::uint32_t bitrate = 320000;
      std::uint32_t sampleRate = 44100;
      std::uint8_t channels = 2;
      std::uint8_t bitDepth = 16;
      ResourceId coverArtId{kInvalidResourceId};
      AudioCodec codec = AudioCodec::Unknown;
      std::uint32_t artistId = 0;
      std::uint32_t albumId = 0;
      std::uint32_t genreId = 0;
      std::uint32_t albumArtistId = 0;
      std::uint32_t composerId = 0;
      std::uint32_t workId = 0;
      std::vector<std::string> tags = {};
      std::vector<std::pair<std::string, std::string>> customPairs = {};
    };

    class TrackFixture final
    {
    public:
      TrackFixture() { init(TrackSpec{}, nullptr); }

      explicit TrackFixture(TrackSpec const& spec, DictionaryStore* dict = nullptr) { init(spec, dict); }

      // Legacy compatibility constructor
      TrackFixture(std::string title,
                   std::string artist = "Test Artist",
                   std::string album = "Test Album",
                   std::string uri = "/path/to/track.flac",
                   std::uint16_t year = 2020,
                   std::uint16_t trackNumber = 5,
                   std::uint32_t durationMillis = 180000,
                   std::uint32_t bitrate = 320000,
                   std::uint32_t sampleRate = 44100,
                   std::uint8_t channels = 2,
                   std::uint8_t bitDepth = 16,
                   std::uint32_t artistId = 0,
                   std::uint32_t albumId = 0,
                   std::uint32_t genreId = 0,
                   std::vector<std::uint32_t> const& tagIds = {},
                   std::string composer = "",
                   std::string work = "")
      {
        auto spec = TrackSpec{};
        spec.title = std::move(title);
        spec.artist = std::move(artist);
        spec.album = std::move(album);
        spec.uri = std::move(uri);
        spec.year = year;
        spec.trackNumber = trackNumber;
        spec.duration = std::chrono::milliseconds{durationMillis};
        spec.bitrate = bitrate;
        spec.sampleRate = sampleRate;
        spec.channels = channels;
        spec.bitDepth = bitDepth;
        spec.artistId = artistId;
        spec.albumId = albumId;
        spec.genreId = genreId;

        for (auto id : tagIds)
        {
          spec.tags.push_back(std::format("tag{}", id));
        }

        spec.composer = std::move(composer);
        spec.work = std::move(work);
        init(spec, nullptr);
      }

      TrackView view() const { return TrackView{_hotData, _coldData}; }
      TrackView hotOnlyView() const { return TrackView{_hotData, std::span<std::byte const>{}}; }
      TrackView coldOnlyView() const { return TrackView{std::span<std::byte const>{}, _coldData}; }
      DictionaryStore& dictionary() { return *_optDict; }

    private:
      void init(TrackSpec const& spec, DictionaryStore* dict)
      {
        auto temp = TempDir{};
        auto envOpts = Environment::Options{.flags = MDB_CREATE, .maxDatabases = 20};
        _optEnv.emplace(openEnvironment(temp.path(), envOpts));
        auto wtxn = beginWriteTransaction(*_optEnv);

        if (dict == nullptr)
        {
          _optDict.emplace(openDatabase(wtxn, "dict"), wtxn);
          dict = &*_optDict;
        }

        _optResources.emplace(openDatabase(wtxn, "resources"));

        TrackBuilder builder = TrackBuilder::createNew();
        builder.metadata().title(spec.title);
        builder.metadata().artist(spec.artist);
        builder.metadata().album(spec.album);
        builder.metadata().albumArtist(spec.albumArtist);
        builder.metadata().composer(spec.composer);
        builder.metadata().work(spec.work);
        builder.metadata().genre(spec.genre);
        builder.metadata().year(spec.year);
        builder.metadata().trackNumber(spec.trackNumber);
        builder.metadata().trackTotal(spec.trackTotal);
        builder.metadata().discNumber(spec.discNumber);
        builder.metadata().discTotal(spec.discTotal);

        builder.property().uri(spec.uri);
        builder.property().duration(spec.duration);
        builder.property().bitrate(Bitrate{spec.bitrate});
        builder.property().sampleRate(SampleRate{spec.sampleRate});
        builder.property().channels(Channels{spec.channels});
        builder.property().bitDepth(BitDepth{spec.bitDepth});
        builder.property().codec(spec.codec);

        if (spec.coverArtId != kInvalidResourceId)
        {
          builder.coverArt().add(PictureType::FrontCover, spec.coverArtId);
        }

        for (auto const& name : spec.tags)
        {
          builder.tags().add(name);
        }

        for (auto const& [k, v] : spec.customPairs)
        {
          builder.customMetadata().add(k, v);
        }

        auto hotDataResult = builder.serializeHot(wtxn, *dict);
        REQUIRE(hotDataResult);
        auto coldDataResult = builder.serializeCold(wtxn, *dict, *_optResources);
        REQUIRE(coldDataResult);
        _hotData = *hotDataResult;
        _coldData = *coldDataResult;

        // Manual ID overrides if specified
        auto* header = utility::layout::asMutablePtr<library::TrackHotHeader>(_hotData);

        if (spec.artistId != 0)
        {
          header->artistId = DictionaryId{spec.artistId};
        }

        if (spec.albumId != 0)
        {
          header->albumId = DictionaryId{spec.albumId};
        }

        if (spec.genreId != 0)
        {
          header->genreId = DictionaryId{spec.genreId};
        }

        if (spec.albumArtistId != 0)
        {
          header->albumArtistId = DictionaryId{spec.albumArtistId};
        }

        if (spec.composerId != 0)
        {
          header->composerId = DictionaryId{spec.composerId};
        }

        auto* coldHeader = utility::layout::asMutablePtr<library::TrackColdHeader>(_coldData);

        if (spec.workId != 0)
        {
          coldHeader->workId = DictionaryId{spec.workId};
        }
      }

      std::optional<Environment> _optEnv;
      std::optional<DictionaryStore> _optDict;
      std::optional<ResourceStore> _optResources;
      std::vector<std::byte> _hotData;
      std::vector<std::byte> _coldData;
    };

    using TestTrack = TrackFixture;

    std::vector<std::byte> makeHotOnlyTrack(DictionaryId artistId = kInvalidDictionaryId,
                                            DictionaryId albumId = kInvalidDictionaryId,
                                            DictionaryId genreId = kInvalidDictionaryId,
                                            DictionaryId albumArtistId = kInvalidDictionaryId,
                                            std::span<DictionaryId const> tagIds = {})
    {
      auto header = library::TrackHotHeader{};
      header.artistId = artistId;
      header.albumId = albumId;
      header.genreId = genreId;
      header.albumArtistId = albumArtistId;
      header.tagLength = static_cast<std::uint16_t>(tagIds.size_bytes());

      for (auto const tagId : tagIds)
      {
        header.tagBloom |= std::uint32_t{1} << (tagId.raw() & 31U);
      }

      auto data = serializeHeader(header);

      for (auto const tagId : tagIds)
      {
        data.insert_range(data.end(), utility::bytes::view(tagId));
      }

      appendString(data, "");
      return data;
    }
  } // namespace

  TEST_CASE("PlanEvaluator - Simple Equal Match", "[query][unit][plan_evaluator]")
  {
    auto expr = parseOk("$year = 2020");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
    auto evaluator = PlanEvaluator{};

    auto track1 = TestTrack{"Test", "Artist", "Album", "/path", 2020};
    auto result = evaluator.evaluateFull(plan, track1.view());
    CHECK(result == true);

    auto track2 = TestTrack{"Test", "Artist", "Album", "/path", 2019};
    result = evaluator.evaluateFull(plan, track2.view());
    CHECK(result == false);
  }

  TEST_CASE("PlanEvaluator - Greater Than", "[query][unit][plan_evaluator]")
  {
    auto expr = parseOk("@duration > 179000");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
    auto evaluator = PlanEvaluator{};

    auto track1 = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 180000};
    auto result = evaluator.evaluateFull(plan, track1.view());
    CHECK(result == true);

    auto track2 = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 170000};
    result = evaluator.evaluateFull(plan, track2.view());
    CHECK(result == false);
  }

  TEST_CASE("PlanEvaluator - NotEqual", "[query][unit][plan_evaluator]")
  {
    auto expr = parseOk("$year != 2020");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
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

  TEST_CASE("PlanEvaluator - Greater Than Or Equal", "[query][unit][plan_evaluator]")
  {
    auto expr = parseOk("@duration >= 180000");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
    auto evaluator = PlanEvaluator{};

    auto track1 = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 180000};
    auto result = evaluator.evaluateFull(plan, track1.view());
    CHECK(result == true);

    auto track2 = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 179999};
    result = evaluator.evaluateFull(plan, track2.view());
    CHECK(result == false);
  }

  TEST_CASE("PlanEvaluator - Less Than", "[query][unit][plan_evaluator]")
  {
    auto expr = parseOk("$year < 2021");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
    auto evaluator = PlanEvaluator{};

    auto track1 = TestTrack{"Test", "Artist", "Album", "/path", 2020};
    auto result = evaluator.evaluateFull(plan, track1.view());
    CHECK(result == true);

    auto track2 = TestTrack{"Test", "Artist", "Album", "/path", 2021};
    result = evaluator.evaluateFull(plan, track2.view());
    CHECK(result == false);
  }

  TEST_CASE("PlanEvaluator - Like Match", "[query][unit][plan_evaluator]")
  {
    auto expr = parseOk("$title ~ Test");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
    auto evaluator = PlanEvaluator{};

    auto track1 = TestTrack{"Test Title"};
    auto result = evaluator.evaluateFull(plan, track1.view());
    CHECK(result == true);

    auto track2 = TestTrack{"Another Title"};
    result = evaluator.evaluateFull(plan, track2.view());
    CHECK(result == false);
  }

  TEST_CASE("PlanEvaluator - Work Metadata", "[query][unit][plan_evaluator]")
  {
    auto trackWithWork = TestTrack{
      "Title", "Artist", "Album", "/path", 2020, 5, 180000, 320000, 44100, 2, 16, 1, 2, 3, {}, "", "Symphony No. 5"};
    auto trackWithoutWork =
      TestTrack{"Title", "Artist", "Album", "/path", 2020, 5, 180000, 320000, 44100, 2, 16, 1, 2, 3, {}, "", ""};
    auto evaluator = PlanEvaluator{};

    SECTION("$work Equality")
    {
      auto expr = parseOk("$work = 'Symphony No. 5'");
      auto compiler = QueryCompiler{&trackWithWork.dictionary()};
      auto plan = compileOk(compiler, expr);
      CHECK(evaluator.evaluateFull(plan, trackWithWork.view()) == true);
      CHECK(evaluator.evaluateFull(plan, trackWithoutWork.view()) == false);
    }

    SECTION("$w Equality (shorthand)")
    {
      auto expr = parseOk("$w = 'Symphony No. 5'");
      auto compiler = QueryCompiler{&trackWithWork.dictionary()};
      auto plan = compileOk(compiler, expr);
      CHECK(evaluator.evaluateFull(plan, trackWithWork.view()) == true);
    }

    SECTION("$work LIKE")
    {
      auto expr = parseOk("$work ~ Symphony");
      auto compiler = QueryCompiler{&trackWithWork.dictionary()};
      auto plan = compileOk(compiler, expr);
      CHECK(evaluator.evaluateFull(plan, trackWithWork.view()) == true);
      CHECK(evaluator.evaluateFull(plan, trackWithoutWork.view()) == false);
    }
  }

  TEST_CASE("PlanEvaluator - Composer Metadata", "[query][unit][plan_evaluator]")
  {
    auto trackWithComposer = TestTrack{
      "Title", "Artist", "Album", "/path", 2020, 5, 180000, 320000, 44100, 2, 16, 1, 2, 3, {}, "Beethoven", ""};
    auto evaluator = PlanEvaluator{};

    SECTION("$composer Equality")
    {
      auto expr = parseOk("$composer = Beethoven");
      auto compiler = QueryCompiler{&trackWithComposer.dictionary()};
      auto plan = compileOk(compiler, expr);
      CHECK(evaluator.evaluateFull(plan, trackWithComposer.view()) == true);
    }

    SECTION("$composer LIKE")
    {
      auto expr = parseOk("$composer ~ Beet");
      auto compiler = QueryCompiler{&trackWithComposer.dictionary()};
      auto plan = compileOk(compiler, expr);
      CHECK(evaluator.evaluateFull(plan, trackWithComposer.view()) == true);
    }
  }

  TEST_CASE("PlanEvaluator - Artist LIKE resolves DictionaryId strings", "[query][unit][plan_evaluator]")
  {
    auto temp = TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});
    auto wtxn = beginWriteTransaction(env);
    auto dict = DictionaryStore{openDatabase(wtxn, "dict"), wtxn};
    auto bachId = ao::test::requireValue(dict.put(wtxn, "Johann Sebastian Bach"));
    auto mozartId = ao::test::requireValue(dict.put(wtxn, "Wolfgang Amadeus Mozart"));

    auto expr = parseOk(R"($artist ~ "Bach")");
    auto compiler = QueryCompiler{&dict};
    auto plan = compileOk(compiler, expr);
    auto evaluator = PlanEvaluator{};

    auto matchingHotData = makeHotOnlyTrack(bachId);
    auto matchingTrack = library::TrackView{matchingHotData, std::span<std::byte const>{}};
    CHECK(evaluator.evaluateFull(plan, matchingTrack) == true);

    auto nonMatchingHotData = makeHotOnlyTrack(mozartId);
    auto nonMatchingTrack = library::TrackView{nonMatchingHotData, std::span<std::byte const>{}};
    CHECK(evaluator.evaluateFull(plan, nonMatchingTrack) == false);

    auto missingArtistHotData = makeHotOnlyTrack();
    auto missingArtistTrack = library::TrackView{missingArtistHotData, std::span<std::byte const>{}};
    CHECK(evaluator.evaluateFull(plan, missingArtistTrack) == false);
  }

  TEST_CASE("PlanEvaluator - OR between artist LIKE and tag does not over-prune on bloom filter",
            "[query][unit][plan_evaluator]")
  {
    auto temp = TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});
    auto wtxn = beginWriteTransaction(env);
    auto dict = DictionaryStore{openDatabase(wtxn, "dict"), wtxn};
    auto aimerId = ao::test::requireValue(dict.put(wtxn, "Aimer"));
    wtxn.commit();

    auto expr = parseOk(R"($artist ~ "Aimer" or #Aimer)");
    auto compiler = QueryCompiler{&dict};
    auto plan = compileOk(compiler, expr);
    auto evaluator = PlanEvaluator{};

    CHECK(plan.tagBloomMask == 0);

    auto artistMatchHotData = makeHotOnlyTrack(aimerId);
    auto artistMatchTrack = library::TrackView{artistMatchHotData, std::span<std::byte const>{}};
    CHECK(evaluator.matches(plan, artistMatchTrack) == true);

    auto tagIds = std::array<DictionaryId, 1>{aimerId};
    auto tagMatchHotData =
      makeHotOnlyTrack(kInvalidDictionaryId, kInvalidDictionaryId, kInvalidDictionaryId, kInvalidDictionaryId, tagIds);
    auto tagMatchTrack = library::TrackView{tagMatchHotData, std::span<std::byte const>{}};
    CHECK(evaluator.matches(plan, tagMatchTrack) == true);

    auto noMatchHotData = makeHotOnlyTrack();
    auto noMatchTrack = library::TrackView{noMatchHotData, std::span<std::byte const>{}};
    CHECK(evaluator.matches(plan, noMatchTrack) == false);
  }

  TEST_CASE("PlanEvaluator - Title String Equal", "[query][unit][plan_evaluator]")
  {
    auto expr = parseOk("$title = 'Hello World'");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
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

  TEST_CASE("PlanEvaluator - Title String Not Equal", "[query][unit][plan_evaluator]")
  {
    auto expr = parseOk("$title != 'Hello'");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
    auto evaluator = PlanEvaluator{};

    auto track1 = TestTrack{"Hello World"};
    auto result = evaluator.evaluateFull(plan, track1.view());
    CHECK(result == true);

    auto track2 = TestTrack{"Hello"};
    result = evaluator.evaluateFull(plan, track2.view());
    CHECK(result == false);
  }

  TEST_CASE("PlanEvaluator - Title String Less Than", "[query][unit][plan_evaluator]")
  {
    auto expr = parseOk("$title < 'zoo'");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
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

  TEST_CASE("PlanEvaluator - Title String Greater Than", "[query][unit][plan_evaluator]")
  {
    auto expr = parseOk("$title > 'apple'");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
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

  TEST_CASE("PlanEvaluator - Logical And", "[query][unit][plan_evaluator]")
  {
    auto expr = parseOk("$year = 2020 && @duration > 100000");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
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

  TEST_CASE("PlanEvaluator - Logical Or", "[query][unit][plan_evaluator]")
  {
    auto expr = parseOk("$year = 2020 || $year = 2019");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
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

  TEST_CASE("PlanEvaluator - Logical Not", "[query][unit][plan_evaluator]")
  {
    // Use "not(" for explicit grouping, or check if parser handles precedence
    auto expr = parseOk("!($year = 2020)");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
    auto evaluator = PlanEvaluator{};

    auto track1 = TestTrack{"Test", "Artist", "Album", "/path", 2020};
    auto result = evaluator.evaluateFull(plan, track1.view());
    CHECK(result == false);

    auto track2 = TestTrack{"Test", "Artist", "Album", "/path", 2019};
    result = evaluator.evaluateFull(plan, track2.view());
    CHECK(result == true);
  }

  TEST_CASE("PlanEvaluator - Complex Expression", "[query][unit][plan_evaluator]")
  {
    // Note: genre comparison requires dictionary resolution, so we use numeric genreId
    auto expr = parseOk("@duration > 180000 && $year >= 2020");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
    auto evaluator = PlanEvaluator{};

    auto track1 = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 200000, 320000, 44100, 2, 16, 1, 2, 1};
    auto result = evaluator.evaluateFull(plan, track1.view());
    CHECK(result == true);

    auto track2 = TestTrack{"Test", "Artist", "Album", "/path", 2019, 5, 200000, 320000, 44100, 2, 16, 1, 2, 1};
    result = evaluator.evaluateFull(plan, track2.view());
    CHECK(result == false);
  }

  TEST_CASE("PlanEvaluator - Matches All", "[query][unit][plan_evaluator]")
  {
    auto expr = parseOk("true");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
    auto evaluator = PlanEvaluator{};

    CHECK(plan.matchesAll == true);

    auto track1 = TestTrack{};
    auto result = evaluator.matches(plan, track1.view());
    CHECK(result == true);

    auto track2 = TestTrack{"Test", "Artist", "Album", "/path", 1900};
    result = evaluator.matches(plan, track2.view());
    CHECK(result == true);
  }

  TEST_CASE("PlanEvaluator - String Constant Out Of Bounds", "[query][unit][plan_evaluator]")
  {
    auto plan = ExecutionPlan{};
    plan.instructions.push_back(
      {.op = OpCode::LoadField, .field = static_cast<std::uint8_t>(Field::Title), .operand = 0});
    plan.instructions.push_back({.op = OpCode::LoadConstant, .operand = 1, .constValue = 999});
    plan.instructions.push_back({.op = OpCode::Eq, .operand = 1}); // out of bounds string index
    plan.instructions.push_back({.op = OpCode::LoadConstant, .operand = 1, .constValue = -1LL});
    plan.instructions.push_back({.op = OpCode::Eq, .operand = 1}); // < 0

    auto evaluator = PlanEvaluator{};
    auto track = TestTrack{"Title"};
    auto result = evaluator.evaluateFull(plan, track.view());
    CHECK(result == false);
  }

  TEST_CASE("PlanEvaluator - Other Dictionary Fields", "[query][unit][plan_evaluator]")
  {
    auto spec = TrackSpec{};
    spec.codec = AudioCodec::Flac;
    spec.trackNumber = 3;
    spec.trackTotal = 12;
    spec.discNumber = 1;
    spec.discTotal = 2;
    spec.coverArtId = ResourceId{99};
    spec.album = "Test Album";
    spec.genre = "Test Genre";
    spec.albumArtist = "Test Album Artist";
    auto track = TestTrack{spec};

    auto& dict = track.dictionary();
    auto compiler = QueryCompiler{&dict};
    auto evaluator = PlanEvaluator{};

    SECTION("Album")
    {
      auto plan = compileOk(compiler, parseOk("$album = 'Test Album'"));
      CHECK(evaluator.evaluateFull(plan, track.view()) == true);

      auto planLike = compileOk(compiler, parseOk("$album ~ 'Test'"));
      CHECK(evaluator.evaluateFull(planLike, track.view()) == true);
    }

    SECTION("Genre")
    {
      auto plan = compileOk(compiler, parseOk("$genre = 'Test Genre'"));
      CHECK(evaluator.evaluateFull(plan, track.view()) == true);

      auto planLike = compileOk(compiler, parseOk("$genre ~ 'Genre'"));
      CHECK(evaluator.evaluateFull(planLike, track.view()) == true);
    }

    SECTION("AlbumArtist")
    {
      auto plan = compileOk(compiler, parseOk("$albumArtist = 'Test Album Artist'"));
      CHECK(evaluator.evaluateFull(plan, track.view()) == true);

      auto planLike = compileOk(compiler, parseOk("$albumArtist ~ 'Album Artist'"));
      CHECK(evaluator.evaluateFull(planLike, track.view()) == true);
    }

    SECTION("Uri")
    {
      auto plan = ExecutionPlan{};
      plan.stringConstants.emplace_back("/path/to/track.flac");
      plan.instructions.push_back(
        {.op = OpCode::LoadField, .field = static_cast<std::uint8_t>(Field::Uri), .operand = 0});
      plan.instructions.push_back({.op = OpCode::LoadConstant, .operand = 1, .constValue = 0});
      plan.instructions.push_back({.op = OpCode::Eq, .operand = 1});
      CHECK(evaluator.evaluateFull(plan, track.view()) == true);
    }

    SECTION("Channels")
    {
      auto plan = compileOk(compiler, parseOk("@channels = 2"));
      CHECK(evaluator.evaluateFull(plan, track.view()) == true);
    }

    SECTION("BitDepth")
    {
      auto plan = compileOk(compiler, parseOk("@bitDepth = 16"));
      CHECK(evaluator.evaluateFull(plan, track.view()) == true);
    }

    SECTION("Codec")
    {
      auto plan = compileOk(compiler, parseOk("@codec = FLAC"));
      CHECK(evaluator.evaluateFull(plan, track.view()) == true);
    }

    SECTION("Numeric Metadata Fields")
    {
      CHECK(evaluator.evaluateFull(compileOk(compiler, parseOk("$trackNumber = 3")), track.view()) == true);
      CHECK(evaluator.evaluateFull(compileOk(compiler, parseOk("$trackTotal = 12")), track.view()) == true);
      CHECK(evaluator.evaluateFull(compileOk(compiler, parseOk("$discNumber = 1")), track.view()) == true);
      CHECK(evaluator.evaluateFull(compileOk(compiler, parseOk("$discTotal = 2")), track.view()) == true);
    }

    SECTION("CoverArtId")
    {
      auto plan = ExecutionPlan{};
      plan.instructions.push_back(
        {.op = OpCode::LoadField, .field = static_cast<std::uint8_t>(Field::CoverArtId), .operand = 0});
      plan.instructions.push_back({.op = OpCode::LoadConstant, .operand = 1, .constValue = 99});
      plan.instructions.push_back({.op = OpCode::Eq, .operand = 1});
      CHECK(evaluator.evaluateFull(plan, track.view()) == true);
    }

    SECTION("TagCount")
    {
      auto plan = ExecutionPlan{};
      plan.instructions.push_back(
        {.op = OpCode::LoadField, .field = static_cast<std::uint8_t>(Field::TagCount), .operand = 0});
      plan.instructions.push_back({.op = OpCode::LoadConstant, .operand = 1, .constValue = 0});
      plan.instructions.push_back({.op = OpCode::Eq, .operand = 1});
      CHECK(evaluator.evaluateFull(plan, track.view()) == true);
    }

    SECTION("Custom Property")
    {
      auto spec2 = TrackSpec{};
      spec2.customPairs.emplace_back("customName", "customValue");
      auto track2 = TestTrack{spec2};

      auto plan = compileOk(QueryCompiler{&track2.dictionary()}, parseOk("%customName = 'customValue'"));
      CHECK(PlanEvaluator{}.evaluateFull(plan, track2.view()) == true);

      // Hit the fallback return {} for Custom when instruction is invalid
      auto planManual = ExecutionPlan{};
      planManual.instructions.push_back(
        {.op = OpCode::LoadField, .field = static_cast<std::uint8_t>(Field::Custom), .operand = 0, .constValue = 0});
      planManual.stringConstants.emplace_back("");
      planManual.instructions.push_back({.op = OpCode::LoadConstant, .operand = 1, .constValue = 0});
      planManual.instructions.push_back({.op = OpCode::Eq, .operand = 1});
      CHECK(PlanEvaluator{}.evaluateFull(planManual, track2.view()) == true);
    }

    SECTION("Invalid Field")
    {
      auto plan = ExecutionPlan{};
      plan.stringConstants.emplace_back("pattern");
      // 255 is an invalid field
      plan.instructions.push_back({.op = OpCode::LoadField, .field = 255, .operand = 0});
      plan.instructions.push_back({.op = OpCode::LoadConstant, .operand = 1, .constValue = 0});
      plan.instructions.push_back({.op = OpCode::Like, .operand = 1});
      CHECK(evaluator.evaluateFull(plan, track.view()) == false);
    }
  }

  TEST_CASE("PlanEvaluator - Invalid Track View", "[query][unit][plan_evaluator]")
  {
    auto expr = parseOk("$year = 2020");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
    auto evaluator = PlanEvaluator{};

    // Empty hot data creates an invalid TrackView
    auto emptyView = library::TrackView{std::span<std::byte const>{}, std::span<std::byte const>{}};
    auto result = evaluator.evaluateFull(plan, emptyView);
    CHECK(result == false);
  }

  TEST_CASE("PlanEvaluator - ColdOnly plan works with cold-only TrackView", "[query][unit][plan_evaluator]")
  {
    auto expr = parseOk("@duration >= 180000");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
    auto evaluator = PlanEvaluator{};

    REQUIRE(plan.accessProfile == AccessProfile::ColdOnly);

    auto track = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 180000};
    CHECK(evaluator.evaluateFull(plan, track.coldOnlyView()) == true);
  }

  TEST_CASE("PlanEvaluator - Mixed plan still requires both storage tiers", "[query][unit][plan_evaluator]")
  {
    auto expr = parseOk("$year = 2020 && @duration >= 180000");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
    auto evaluator = PlanEvaluator{};

    REQUIRE(plan.accessProfile == AccessProfile::HotAndCold);

    auto track = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 180000};
    CHECK(evaluator.evaluateFull(plan, track.hotOnlyView()) == false);
    CHECK(evaluator.evaluateFull(plan, track.coldOnlyView()) == false);
  }

  TEST_CASE("PlanEvaluator - Bitrate Comparison", "[query][unit][plan_evaluator]")
  {
    auto expr = parseOk("@bitrate >= 320000");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
    auto evaluator = PlanEvaluator{};

    auto track1 = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 180000, 320000};
    auto result = evaluator.evaluateFull(plan, track1.view());
    CHECK(result == true);

    auto track2 = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 180000, 256000};
    result = evaluator.evaluateFull(plan, track2.view());
    CHECK(result == false);
  }

  TEST_CASE("PlanEvaluator - SampleRate Comparison", "[query][unit][plan_evaluator]")
  {
    auto expr = parseOk("@sampleRate >= 48000");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
    auto evaluator = PlanEvaluator{};

    auto track1 = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 180000, 320000, 48000};
    auto result = evaluator.evaluateFull(plan, track1.view());
    CHECK(result == true);

    auto track2 = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 180000, 320000, 44100};
    result = evaluator.evaluateFull(plan, track2.view());
    CHECK(result == false);
  }

  TEST_CASE("PlanEvaluator - Unit Constants", "[query][unit][plan_evaluator]")
  {
    auto expr = parseOk("@duration >= 3m && @bitrate >= 256k && @sampleRate >= 44.1k");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
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

  TEST_CASE("PlanEvaluator - Year Comparison", "[query][unit][plan_evaluator]")
  {
    // Note: $trackNumber is a cold field, so we use $year instead which is hot
    auto expr = parseOk("$year = 2020");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
    auto evaluator = PlanEvaluator{};

    auto track1 = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5};
    auto result = evaluator.evaluateFull(plan, track1.view());
    CHECK(result == true);

    auto track2 = TestTrack{"Test", "Artist", "Album", "/path", 2019, 5};
    result = evaluator.evaluateFull(plan, track2.view());
    CHECK(result == false);
  }

  TEST_CASE("PlanEvaluator - Tag Query - No Tags", "[query][unit][plan_evaluator]")
  {
    // Query for tag - with dictionary resolving "rock" -> ID 10
    auto expr = parseOk("#rock");
    auto compiler = QueryCompiler{}; // No dictionary - tag resolution disabled
    auto plan = compileOk(compiler, expr);
    auto evaluator = PlanEvaluator{};

    // Track with no tags - bloom filter rejects
    auto track1 = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 180000, 320000, 44100, 2, 16, 1, 2, 3, {}};
    auto result = evaluator.matches(plan, track1.view());
    CHECK(result == false);
  }

  TEST_CASE("PlanEvaluator - Tag Query - With Matching Tag", "[query][unit][plan_evaluator]")
  {
    // Set up DictionaryStore with tag
    auto temp = TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto dict = DictionaryStore{openDatabase(wtxn, "dict"), wtxn};
    REQUIRE(dict.put(wtxn, "rock")); // Will get ID 1 (DictionaryId starts from 1)
    wtxn.commit();

    // Query for #rock
    auto expr = parseOk("#rock");
    auto compiler = QueryCompiler{&dict}; // Pass dictionary for tag resolution
    auto plan = compileOk(compiler, expr);
    auto evaluator = PlanEvaluator{};

    // Track with tag ID 0 (matches "rock" = 0)
    auto trackWithTag =
      TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 180000, 320000, 44100, 2, 16, 1, 2, 3, {0}};
    auto result = evaluator.matches(plan, trackWithTag.view());
    CHECK(result == true);
  }

  TEST_CASE("PlanEvaluator - Numeric Tag And Quoted Custom Key", "[query][unit][plan_evaluator]")
  {
    auto spec = TrackSpec{};
    spec.tags.emplace_back("123");
    spec.customPairs.emplace_back("Replay Gain", "high");
    auto track = TestTrack{spec};

    auto const expression = parseOk(R"(#123 and %"Replay Gain" = "high")");
    auto const plan = compileOk(QueryCompiler{&track.dictionary()}, expression);

    CHECK(PlanEvaluator{}.evaluateFull(plan, track.view()));
  }

  TEST_CASE("PlanEvaluator - Existence Tests", "[query][unit][plan_evaluator]")
  {
    auto evaluator = PlanEvaluator{};

    SECTION("StringMetadataExistsWhenNonEmpty")
    {
      auto missingSpec = TrackSpec{};
      missingSpec.title.clear();
      auto missing = TrackFixture{missingSpec};
      auto present = TrackFixture{TrackSpec{}};
      auto plan = compileOk(QueryCompiler{}, parseOk("$title?"));

      CHECK_FALSE(evaluator.evaluateFull(plan, missing.view()));
      CHECK(evaluator.evaluateFull(plan, present.view()));
    }

    SECTION("DictionaryMetadataExistsWhenIdIsValid")
    {
      auto missingSpec = TrackSpec{};
      missingSpec.artist.clear();
      auto missing = TrackFixture{missingSpec};
      auto present = TrackFixture{TrackSpec{}};
      auto plan = compileOk(QueryCompiler{}, parseOk("$artist?"));

      CHECK_FALSE(evaluator.evaluateFull(plan, missing.view()));
      CHECK(evaluator.evaluateFull(plan, present.view()));
    }

    SECTION("NumericMetadataExistsWhenPositive")
    {
      auto missingSpec = TrackSpec{};
      missingSpec.year = 0;
      missingSpec.trackNumber = 0;
      missingSpec.trackTotal = 0;
      auto missing = TrackFixture{missingSpec};

      auto presentSpec = TrackSpec{};
      presentSpec.year = 2024;
      presentSpec.trackNumber = 3;
      presentSpec.trackTotal = 12;
      auto present = TrackFixture{presentSpec};

      CHECK_FALSE(evaluator.evaluateFull(compileOk(QueryCompiler{}, parseOk("$year?")), missing.view()));
      CHECK(evaluator.evaluateFull(compileOk(QueryCompiler{}, parseOk("$year?")), present.view()));
      CHECK_FALSE(evaluator.evaluateFull(compileOk(QueryCompiler{}, parseOk("$trackNumber?")), missing.view()));
      CHECK(evaluator.evaluateFull(compileOk(QueryCompiler{}, parseOk("$trackNumber?")), present.view()));
      CHECK_FALSE(evaluator.evaluateFull(compileOk(QueryCompiler{}, parseOk("$trackTotal?")), missing.view()));
      CHECK(evaluator.evaluateFull(compileOk(QueryCompiler{}, parseOk("$trackTotal?")), present.view()));
    }

    SECTION("PropertiesExistWhenPositiveOrKnown")
    {
      auto missingSpec = TrackSpec{};
      missingSpec.duration = std::chrono::milliseconds{0};
      missingSpec.codec = AudioCodec::Unknown;
      auto missing = TrackFixture{missingSpec};

      auto presentSpec = TrackSpec{};
      presentSpec.duration = std::chrono::milliseconds{1};
      presentSpec.codec = AudioCodec::Flac;
      auto present = TrackFixture{presentSpec};

      CHECK_FALSE(evaluator.evaluateFull(compileOk(QueryCompiler{}, parseOk("@duration?")), missing.view()));
      CHECK(evaluator.evaluateFull(compileOk(QueryCompiler{}, parseOk("@duration?")), present.view()));
      CHECK_FALSE(evaluator.evaluateFull(compileOk(QueryCompiler{}, parseOk("@codec?")), missing.view()));
      CHECK(evaluator.evaluateFull(compileOk(QueryCompiler{}, parseOk("@codec?")), present.view()));
    }

    SECTION("CoverArtExistsWhenPrimaryResourceIsValid")
    {
      auto missing = TrackFixture{TrackSpec{}};
      auto presentSpec = TrackSpec{};
      presentSpec.coverArtId = ResourceId{42};
      auto present = TrackFixture{presentSpec};
      auto plan = compileOk(QueryCompiler{}, parseOk("$coverArt?"));

      CHECK_FALSE(evaluator.evaluateFull(plan, missing.view()));
      CHECK(evaluator.evaluateFull(plan, present.view()));
    }

    SECTION("CustomMetadataExistsEvenWhenValueIsEmpty")
    {
      auto absent = TrackFixture{TrackSpec{}};
      auto emptyValueSpec = TrackSpec{};
      emptyValueSpec.customPairs.emplace_back("rating", "");
      auto emptyValue = TrackFixture{emptyValueSpec};
      auto nonEmptyValueSpec = TrackSpec{};
      nonEmptyValueSpec.customPairs.emplace_back("rating", "5");
      auto nonEmptyValue = TrackFixture{nonEmptyValueSpec};

      auto plan = compileOk(QueryCompiler{&emptyValue.dictionary()}, parseOk("%rating?"));

      CHECK_FALSE(evaluator.evaluateFull(plan, absent.view()));
      CHECK(evaluator.evaluateFull(plan, emptyValue.view()));
      CHECK(evaluator.evaluateFull(plan, nonEmptyValue.view()));
    }

    SECTION("TagExistenceMatchesMembership")
    {
      auto absent = TrackFixture{TrackSpec{}};
      auto presentSpec = TrackSpec{};
      presentSpec.tags.emplace_back("favorite");
      auto present = TrackFixture{presentSpec};
      auto plan = compileOk(QueryCompiler{&present.dictionary()}, parseOk("#favorite?"));

      CHECK_FALSE(evaluator.evaluateFull(plan, absent.view()));
      CHECK(evaluator.evaluateFull(plan, present.view()));
    }

    SECTION("NegatedExistenceMatchesMissingFields")
    {
      auto missingSpec = TrackSpec{};
      missingSpec.year = 0;
      auto missing = TrackFixture{missingSpec};
      auto present = TrackFixture{TrackSpec{}};
      auto plan = compileOk(QueryCompiler{}, parseOk("!$year?"));

      CHECK(evaluator.evaluateFull(plan, missing.view()));
      CHECK_FALSE(evaluator.evaluateFull(plan, present.view()));
    }
  }

  TEST_CASE("PlanEvaluator - In List", "[query][unit][plan_evaluator]")
  {
    auto spec = TrackSpec{};
    spec.artist = "Bach";
    spec.year = 1990;
    spec.duration = std::chrono::minutes{3};
    spec.customPairs.emplace_back("mood", "focus");
    auto track = TrackFixture{spec};

    auto evaluator = PlanEvaluator{};
    auto compiler = QueryCompiler{&track.dictionary()};

    SECTION("DictionaryBackedStringMatch")
    {
      auto plan = compileOk(compiler, parseOk(R"($artist in ["Bach", "Mozart"])"));
      CHECK(evaluator.evaluateFull(plan, track.view()));
    }

    SECTION("NumericNonMatch")
    {
      auto plan = compileOk(compiler, parseOk("$year in [1988, 1989]"));
      CHECK_FALSE(evaluator.evaluateFull(plan, track.view()));
    }

    SECTION("UnitConstantMatch")
    {
      auto plan = compileOk(compiler, parseOk("@duration in [2m, 3m]"));
      CHECK(evaluator.evaluateFull(plan, track.view()));
    }

    SECTION("CustomStringMatch")
    {
      auto plan = compileOk(compiler, parseOk(R"(%mood in ["study", "focus"])"));
      CHECK(evaluator.evaluateFull(plan, track.view()));
    }

    SECTION("LargeNumericListMatch")
    {
      auto plan = compileOk(compiler, parseOk("$year in [1984, 1985, 1986, 1987, 1988, 1989, 1990, 1991]"));

      REQUIRE(plan.inSets.size() == 1);
      CHECK(evaluator.evaluateFull(plan, track.view()));
    }

    SECTION("LargeDictionaryBackedStringListMatch")
    {
      auto plan = compileOk(
        compiler, parseOk(R"($artist in ["Adams", "Bach", "Chopin", "Debussy", "Elgar", "Faure", "Glass", "Haydn"])"));

      REQUIRE(plan.inSets.size() == 1);
      CHECK(evaluator.evaluateFull(plan, track.view()));
    }

    SECTION("LargeCustomStringListMatch")
    {
      auto plan = compileOk(
        compiler, parseOk(R"(%mood in ["ambient", "deep", "focus", "late", "mix", "quiet", "study", "warm"])"));

      REQUIRE(plan.inSets.size() == 1);
      CHECK(evaluator.evaluateFull(plan, track.view()));
    }

    SECTION("LargeListNonMatch")
    {
      auto plan = compileOk(compiler, parseOk("$year in [1980, 1981, 1982, 1983, 1984, 1985, 1986, 1987]"));

      REQUIRE(plan.inSets.size() == 1);
      CHECK_FALSE(evaluator.evaluateFull(plan, track.view()));
    }
  }

  TEST_CASE("PlanEvaluator - In Range", "[query][unit][plan_evaluator]")
  {
    auto spec = TrackSpec{};
    spec.year = 1994;
    spec.duration = std::chrono::minutes{3};
    auto track = TrackFixture{spec};

    auto evaluator = PlanEvaluator{};
    auto compiler = QueryCompiler{&track.dictionary()};

    SECTION("NumericRangeMatch")
    {
      auto plan = compileOk(compiler, parseOk("$year in 1990..1999"));
      CHECK(evaluator.evaluateFull(plan, track.view()));
    }

    SECTION("UnitRangeMatch")
    {
      auto plan = compileOk(compiler, parseOk("@duration in 2m30s..5m"));
      CHECK(evaluator.evaluateFull(plan, track.view()));
    }

    SECTION("OutOfRangeDoesNotMatch")
    {
      auto plan = compileOk(compiler, parseOk("$year in 1980..1989"));
      CHECK_FALSE(evaluator.evaluateFull(plan, track.view()));
    }
  }

  TEST_CASE("PlanEvaluator - Dictionary Field Lexicographic Comparison", "[query][unit][plan_evaluator]")
  {
    auto temp = TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});
    auto wtxn = beginWriteTransaction(env);
    auto dict = DictionaryStore{openDatabase(wtxn, "dict"), wtxn};

    // Intern in non-alphabetical order so dictionary-ID order differs from text
    // order; a correct comparison must use the resolved text, not the interned ID.
    auto zappaId = ao::test::requireValue(dict.put(wtxn, "Zappa"));
    auto adeleId = ao::test::requireValue(dict.put(wtxn, "Adele"));
    auto mozartId = ao::test::requireValue(dict.put(wtxn, "Mozart"));
    auto kinksId = ao::test::requireValue(dict.put(wtxn, "Kinks")); // sorts strictly between Adele and Mozart

    auto compiler = QueryCompiler{&dict};
    auto evaluator = PlanEvaluator{};

    auto adeleData = makeHotOnlyTrack(adeleId);
    auto adele = library::TrackView{adeleData, std::span<std::byte const>{}};
    auto mozartData = makeHotOnlyTrack(mozartId);
    auto mozart = library::TrackView{mozartData, std::span<std::byte const>{}};
    auto zappaData = makeHotOnlyTrack(zappaId);
    auto zappa = library::TrackView{zappaData, std::span<std::byte const>{}};
    auto kinksData = makeHotOnlyTrack(kinksId);
    auto kinks = library::TrackView{kinksData, std::span<std::byte const>{}};

    SECTION("RangeMatchesByText")
    {
      auto plan = compileOk(compiler, parseOk("$artist in Adele..Mozart"));
      CHECK(evaluator.evaluateFull(plan, adele));       // lower bound, inclusive
      CHECK(evaluator.evaluateFull(plan, kinks));       // strictly inside the range
      CHECK(evaluator.evaluateFull(plan, mozart));      // upper bound, inclusive
      CHECK_FALSE(evaluator.evaluateFull(plan, zappa)); // "Zappa" > "Mozart"
    }

    SECTION("GreaterThanComparesByText")
    {
      auto plan = compileOk(compiler, parseOk("$artist > Mozart"));
      CHECK(evaluator.evaluateFull(plan, zappa));
      CHECK_FALSE(evaluator.evaluateFull(plan, adele));
      CHECK_FALSE(evaluator.evaluateFull(plan, mozart)); // strictly greater
    }

    SECTION("LessThanComparesByText")
    {
      auto plan = compileOk(compiler, parseOk("$artist < Mozart"));
      CHECK(evaluator.evaluateFull(plan, adele));
      CHECK(evaluator.evaluateFull(plan, kinks));
      CHECK_FALSE(evaluator.evaluateFull(plan, mozart)); // strictly less
      CHECK_FALSE(evaluator.evaluateFull(plan, zappa));
    }

    SECTION("LessOrEqualComparesByText")
    {
      auto plan = compileOk(compiler, parseOk("$artist <= Mozart"));
      CHECK(evaluator.evaluateFull(plan, adele));
      CHECK(evaluator.evaluateFull(plan, mozart)); // inclusive
      CHECK_FALSE(evaluator.evaluateFull(plan, zappa));
    }

    SECTION("GreaterOrEqualComparesByText")
    {
      auto plan = compileOk(compiler, parseOk("$artist >= Mozart"));
      CHECK(evaluator.evaluateFull(plan, mozart)); // inclusive
      CHECK(evaluator.evaluateFull(plan, zappa));
      CHECK_FALSE(evaluator.evaluateFull(plan, adele));
    }

    SECTION("NotEqualStillComparesById")
    {
      // '!=' is not an ordered comparison, so it keeps the cheap interned-ID
      // path; this guards the branch executeComparison routes around.
      auto plan = compileOk(compiler, parseOk("$artist != Mozart"));
      CHECK(evaluator.evaluateFull(plan, adele));
      CHECK(evaluator.evaluateFull(plan, zappa));
      CHECK_FALSE(evaluator.evaluateFull(plan, mozart));
    }

    SECTION("ResolvesPerFieldNotJustArtist")
    {
      // Prove the field dispatch in loadDictionaryFieldValue works for a
      // dictionary field other than artist.
      auto rockData = makeHotOnlyTrack(kInvalidDictionaryId, kInvalidDictionaryId, zappaId);
      auto rock = library::TrackView{rockData, std::span<std::byte const>{}};
      auto jazzData = makeHotOnlyTrack(kInvalidDictionaryId, kInvalidDictionaryId, adeleId);
      auto jazz = library::TrackView{jazzData, std::span<std::byte const>{}};

      auto plan = compileOk(compiler, parseOk("$genre > Mozart"));
      CHECK(evaluator.evaluateFull(plan, rock));       // "Zappa" > "Mozart"
      CHECK_FALSE(evaluator.evaluateFull(plan, jazz)); // "Adele" < "Mozart"
    }
  }

  TEST_CASE("PlanEvaluator - Tag Query - With Non-Matching Tag", "[query][unit][plan_evaluator]")
  {
    // Dictionary: "rock" -> ID 10, but track has tag ID 20
    auto expr = parseOk("#rock");
    auto compiler = QueryCompiler{}; // No dictionary - tag resolution disabled
    auto plan = compileOk(compiler, expr);
    auto evaluator = PlanEvaluator{};

    // Track with tag ID 20 (doesn't match "rock" = 10)
    auto trackWithTag =
      TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 180000, 320000, 44100, 2, 16, 1, 2, 3, {20}};
    auto result = evaluator.matches(plan, trackWithTag.view());
    CHECK(result == false);
  }

  TEST_CASE("PlanEvaluator - Tag Field Compilation", "[query][unit][plan_evaluator]")
  {
    // Test that tag field compiles to Field::Tag
    auto expr = parseOk("#tagname");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    // Check that instructions contain LoadField with Field::Tag (16)
    CHECK(!plan.instructions.empty());
    CHECK(plan.instructions[0].op == OpCode::LoadField);
  }

  TEST_CASE("PlanEvaluator - Tag Bloom Filter - Without Dictionary", "[query][unit][plan_evaluator]")
  {
    // Without a dictionary, tag bloom mask cannot be set (no way to resolve tag name to ID)
    auto expr = parseOk("#mytag");
    auto compiler = QueryCompiler{}; // No dictionary provided
    auto plan = compileOk(compiler, expr);

    // Without dictionary, tagBloomMask stays 0
    CHECK(plan.tagBloomMask == 0);
  }

  TEST_CASE("PlanEvaluator - Tag Bloom Filter - Dictionary Miss", "[query][unit][plan_evaluator]")
  {
    // Dictionary has "rock" -> ID 10, but query is for "jazz" which is not in dict
    auto expr = parseOk("#jazz");
    auto compiler = QueryCompiler{}; // No dictionary - tag resolution disabled
    auto plan = compileOk(compiler, expr);

    // "jazz" not in dictionary, tagBloomMask stays 0
    CHECK(plan.tagBloomMask == 0);
  }

  TEST_CASE("PlanEvaluator - Tag Bloom Filter - Dictionary Hit", "[query][unit][plan_evaluator]")
  {
    // Dictionary has "rock" -> ID 10 (10 & 31 = 10)
    auto expr = parseOk("#rock");
    auto compiler = QueryCompiler{}; // No dictionary - tag resolution disabled
    auto plan = compileOk(compiler, expr);

    // Without dictionary, tag resolution doesn't happen, bloom mask stays 0
    CHECK(plan.tagBloomMask == 0);
  }

  TEST_CASE("PlanEvaluator - Tag Bloom Filter - Multiple Tags Hit", "[query][unit][plan_evaluator]")
  {
    // Dictionary: "rock" -> ID 10, "jazz" -> ID 20
    auto expr = parseOk("#rock && #jazz");
    auto compiler = QueryCompiler{}; // No dictionary - tag resolution disabled
    auto plan = compileOk(compiler, expr);

    // Without dictionary, tag resolution doesn't happen, bloom mask stays 0
    CHECK(plan.tagBloomMask == 0);
  }

  TEST_CASE("PlanEvaluator - Tag Bloom Filter - Track Computation", "[query][unit][plan_evaluator]")
  {
    // Test that track bloom is computed correctly: tagId & 31
    // Using manual header construction.
    // Tag ID 10 -> bit 10 (10 & 31 = 10)
    {
      auto h = library::TrackHotHeader{};
      h.tagBloom = (1U << (10 & 31)); // bit 10
      auto data = serializeHeader(h);
      data.push_back(static_cast<std::byte>('\0')); // empty title
      auto view = library::TrackView{data, std::span<std::byte const>{}};
      CHECK(view.tags().bloom() == (1U << 10));
    }

    // Tag ID 32 -> bit 0 (32 & 31 = 0)
    {
      auto h = library::TrackHotHeader{};
      h.tagBloom = (1U << (32 & 31)); // bit 0
      auto data = serializeHeader(h);
      data.push_back(static_cast<std::byte>('\0')); // empty title
      auto view = library::TrackView{data, std::span<std::byte const>{}};
      CHECK(view.tags().bloom() == 1U);
    }

    // Multiple tags: ID 5 and ID 20
    {
      auto h = library::TrackHotHeader{};
      h.tagBloom = (1U << (5 & 31)) | (1U << (20 & 31)); // bits 5 and 20
      auto data = serializeHeader(h);
      data.push_back(static_cast<std::byte>('\0')); // empty title
      auto view = library::TrackView{data, std::span<std::byte const>{}};
      CHECK((view.tags().bloom() & (1U << 5)) != 0);  // Bit 5 should be set
      CHECK((view.tags().bloom() & (1U << 20)) != 0); // Bit 20 should be set
    }
  }

  TEST_CASE("PlanEvaluator - Bloom Filter Fast Path - No Match", "[query][unit][plan_evaluator]")
  {
    auto expr = parseOk("#mytag");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
    auto evaluator = PlanEvaluator{};

    // Create a track with tag bloom bit 0 set (simulating tag ID 32)
    auto h = library::TrackHotHeader{};
    h.tagBloom = 0x00000001U; // Only bit 0 set

    auto data = std::vector<std::byte>{};
    data.insert_range(data.end(), utility::bytes::view(h));

    data.push_back(static_cast<std::byte>('\0')); // empty title
    data.push_back(static_cast<std::byte>('\0')); // empty uri

    auto view = library::TrackView{data, std::span<std::byte const>{}};

    // Bloom filter rejects because query mask doesn't match track bloom
    auto result = evaluator.matches(plan, view);
    CHECK(result == false);
  }

  TEST_CASE("PlanEvaluator - Bloom Filter Fast Path - Match", "[query][unit][plan_evaluator]")
  {
    auto expr = parseOk("#mytag");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
    auto evaluator = PlanEvaluator{};

    // Create a track with tag bloom that has some bits set
    // (without dictionary, mask is 0, so this won't actually test fast path)
    auto h = library::TrackHotHeader{};
    h.tagBloom = 0xFFFFFFFFU; // All bits set

    auto data = std::vector<std::byte>{};
    data.insert_range(data.end(), utility::bytes::view(h));

    data.push_back(static_cast<std::byte>('\0')); // empty title
    data.push_back(static_cast<std::byte>('\0')); // empty uri

    auto view = library::TrackView{data, std::span<std::byte const>{}};

    // With mask 0, bloom check passes (no filtering), falls through to full eval
    auto result = evaluator.matches(plan, view);
    // No tags in track, so should be false
    CHECK(result == false);
  }

  TEST_CASE("PlanEvaluator - Title LIKE with quoted string evaluates correctly", "[query][unit][plan_evaluator]")
  {
    // Simple title LIKE test with quoted string
    auto expr = parseOk(R"($title ~ "Bach")");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
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

  TEST_CASE("PlanEvaluator - Title LIKE with multi-arg Track", "[query][unit][plan_evaluator]")
  {
    // Test LIKE with a Track that has multiple fields set
    auto expr = parseOk(R"($title ~ "Bach")");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
    auto evaluator = PlanEvaluator{};

    auto track = TestTrack{"Bach Greatest Hits", "Artist", "Album", "/path", 2021};
    auto result = evaluator.evaluateFull(plan, track.view());
    CHECK(result == true);
  }

  TEST_CASE("PlanEvaluator - OR expression with LIKE evaluates correctly", "[query][unit][plan_evaluator]")
  {
    // Test that $title ~ "Bach" or $year > 2021 evaluates correctly
    auto expr = parseOk(R"($title ~ "Bach" or $year > 2021)");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
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

  TEST_CASE("PlanEvaluator - OR with simple expressions", "[query][unit][plan_evaluator]")
  {
    // Test $year > 2000 or $year > 1990 to verify OR works with two numeric comparisons
    auto expr = parseOk("$year > 2000 or $year > 1990");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
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

  TEST_CASE("PlanEvaluator - Greater alone for year 1980", "[query][unit][plan_evaluator]")
  {
    // Verify $year > 2000 returns false for year 1980
    auto expr = parseOk("$year > 2000");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
    auto evaluator = PlanEvaluator{};

    auto track = TestTrack{"Title", "Artist", "Album", "/path", 1980};
    auto result = evaluator.evaluateFull(plan, track.view());
    CHECK(result == false);
  }

  TEST_CASE("PlanEvaluator - Year Greater Alone Works", "[query][unit][plan_evaluator]")
  {
    // Simple year > test to verify year comparison works
    auto expr = parseOk("$year > 2000");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
    auto evaluator = PlanEvaluator{};

    auto track1 = TestTrack{"Title", "Artist", "Album", "/path", 2021};
    auto result = evaluator.evaluateFull(plan, track1.view());
    CHECK(result == true);

    auto track2 = TestTrack{"Title", "Artist", "Album", "/path", 1990};
    result = evaluator.evaluateFull(plan, track2.view());
    CHECK(result == false);
  }

  TEST_CASE("PlanEvaluator - Custom Metadata Fields", "[query][unit][plan_evaluator]")
  {
    auto const spec = TrackSpec{.customPairs = {{"isrc", "US-RC1-12-00001"}, {"label", "Deutsche Grammophon"}}};

    auto track = TrackFixture{spec};
    auto evaluator = PlanEvaluator{};
    auto compiler = QueryCompiler{&track.dictionary()};

    SECTION("Custom Field Equality Match")
    {
      auto plan = compileOk(compiler, parseOk("%isrc = 'US-RC1-12-00001'"));
      CHECK(evaluator.evaluateFull(plan, track.view()) == true);
    }

    SECTION("Custom Field Equality NonMatch")
    {
      auto plan = compileOk(compiler, parseOk("%isrc = 'UK-XYZ'"));
      CHECK(evaluator.evaluateFull(plan, track.view()) == false);
    }

    SECTION("Custom Field Like Match")
    {
      auto plan = compileOk(compiler, parseOk("%label ~ 'Grammophon'"));
      CHECK(evaluator.evaluateFull(plan, track.view()) == true);
    }

    SECTION("Custom Field Missing")
    {
      auto plan = compileOk(compiler, parseOk("%nonexistent = 'val'"));
      CHECK(evaluator.evaluateFull(plan, track.view()) == false);
    }
  }

  TEST_CASE("PlanEvaluator - Dictionary-Backed LIKE - Partial Matches", "[query][unit][plan_evaluator]")
  {
    auto const spec = TrackSpec{.artist = "Johann Sebastian Bach"};
    auto track = TrackFixture{spec};
    auto evaluator = PlanEvaluator{};

    // LIKE should work even if we provide a dictionary, but it shouldn't
    // resolve "Bach" to a numeric ID because LIKE requires string scanning.
    auto compiler = QueryCompiler{&track.dictionary()};
    auto plan = compileOk(compiler, parseOk("$artist ~ 'Bach'"));

    CHECK(evaluator.evaluateFull(plan, track.view()) == true);
  }

  TEST_CASE("PlanEvaluator - Unit Scaling Logic", "[query][unit][plan_evaluator]")
  {
    auto const spec = TrackSpec{
      .duration = std::chrono::minutes{3} + std::chrono::seconds{5}, // 3m 5s
      .bitrate = 320000                                              // 320k
    };

    auto track = TrackFixture{spec};
    auto evaluator = PlanEvaluator{};
    auto compiler = QueryCompiler{};

    CHECK(evaluator.evaluateFull(compileOk(compiler, parseOk("@duration > 3m")), track.view()) == true);
    CHECK(evaluator.evaluateFull(compileOk(compiler, parseOk("@duration > 4m")), track.view()) == false);
    CHECK(evaluator.evaluateFull(compileOk(compiler, parseOk("@bitrate = 320k")), track.view()) == true);
  }

  TEST_CASE("PlanEvaluator - Bloom Filter Advanced Scenarios", "[query][unit][plan_evaluator]")
  {
    auto const spec = TrackSpec{.tags = {"rock", "jazz", "blues"}};
    auto track = TrackFixture{spec};
    auto const evaluator = PlanEvaluator{};

    // Need dictionary for compiler to generate bloom mask
    auto compiler = QueryCompiler{&track.dictionary()};

    SECTION("Multi-Tag AND Requires All Bits")
    {
      // #rock and #jazz -> mask should have bits for both
      auto const plan = compileOk(compiler, parseOk("#rock and #jazz"));
      CHECK(plan.tagBloomMask != 0);

      // Should match track with both tags
      CHECK(evaluator.matches(plan, track.view()) == true);

      // Should NOT match track with only one tag
      auto const spec2 = TrackSpec{.tags = {"rock"}};
      auto track2 = TrackFixture{spec2, &track.dictionary()};
      CHECK(evaluator.matches(plan, track2.view()) == false);
    }

    SECTION("Bloom Filter Collision - False Positive Mitigation")
    {
      // The Bloom filter uses (Id % 32) as the bit index.
      // We want two tags: A (in track) and B (not in track) where (A % 32 == B % 32).

      auto& dict = track.dictionary();

      // 1. Find or create a base tag
      auto const* tagA = "rock";
      auto idA = dict.getOrIntern(tagA).raw();
      auto bitIndex = idA % 32;

      // 2. Find a colliding tag B
      auto tagB = std::string{};

      for (std::int32_t i = 0; i < 1000; ++i)
      {
        auto const candidate = std::format("collision_tag_{}", i);

        if (auto const idB = dict.getOrIntern(candidate).raw(); idB != idA && (idB % 32) == bitIndex)
        {
          tagB = candidate;
          break;
        }
      }

      REQUIRE(!tagB.empty());

      // 3. Create a track with ONLY tagA
      auto const spec = TrackSpec{.tags = {tagA}};
      auto trackA = TrackFixture{spec, &dict};

      // 4. Query for tagB
      auto planB = compileOk(compiler, parseOk("#" + tagB));

      // VERIFICATION:
      // The bloom mask for tagB will match tagA's bits (due to collision).
      // evaluator.matches() MUST still return false because it does a full check.
      CHECK(evaluator.matches(planB, trackA.view()) == false);
    }
  }

  TEST_CASE("PlanEvaluator - AAC codec expression", "[query][unit][plan_evaluator]")
  {
    auto spec = TrackSpec{};
    spec.codec = AudioCodec::Aac;
    auto track = TestTrack{spec};

    auto compiler = QueryCompiler{&track.dictionary()};
    auto evaluator = PlanEvaluator{};
    auto plan = compileOk(compiler, parseOk("@codec = AAC"));

    CHECK(evaluator.evaluateFull(plan, track.view()) == true);
  }
} // namespace ao::query::test
