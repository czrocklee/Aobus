// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/query/ExecutionPlanTestSupport.h"
#include "test/unit/query/PlanEvaluatorTestSupport.h"
#include <ao/AudioCodec.h>
#include <ao/library/Credits.h>
#include <ao/query/PlanEvaluator.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <format>
#include <string_view>
#include <tuple>

namespace ao::query::test
{
  namespace
  {
    bool matches(std::string_view const expression, TrackFixture& fixture)
    {
      auto const plan = compileOk(parseOk(expression));
      auto evaluator = PlanEvaluator{};
      return matchesFullPlanWithDictionary(evaluator, plan, fixture.view(), fixture.dictionary());
    }
  } // namespace

  TEST_CASE("PredicateOperandContext - sibling constants do not inherit codec context", "[query][unit][predicate]")
  {
    auto flac = TrackFixture{TrackSpec{.codec = AudioCodec::Flac}};
    auto mp3 = TrackFixture{TrackSpec{.codec = AudioCodec::Mp3}};

    // A preceding codec comparison must not parse the next comparison's literals as codecs.
    CHECK(matches("@codec='FLAC' and 'x'='x'", flac));
    CHECK_FALSE(matches("@codec='FLAC' and 'x'='x'", mp3));
    CHECK_FALSE(matches("@codec = 'FLAC' and 'x' = 'y'", flac));
    CHECK_FALSE(matches("@codec = 'flac' and 'FLAC' = 'flac'", flac));
    CHECK(matches("@codec = 'MP3' or 'x' = 'x'", flac));
    CHECK(matches("@codec = 'FLAC' and 'x' in ['y', 'x']", flac));
    CHECK_FALSE(matches("@codec = 'FLAC' and 'x' in ['y', 'x']", mp3));
  }

  TEST_CASE("PredicateOperandContext - sibling constants do not inherit dictionary context", "[query][unit][predicate]")
  {
    auto bach = TrackFixture{TrackSpec{.artist = "Bach", .genre = "Jazz"}};
    auto mozart = TrackFixture{TrackSpec{.artist = "Mozart", .genre = "Classical"}};

    CHECK(matches("$artist = 'Bach' and 'x' = 'x'", bach));
    CHECK_FALSE(matches("$artist = 'Bach' and 'x' = 'x'", mozart));
    CHECK(matches("$artist = 'Bach' and 'Bach' = 'Bach'", bach));
    CHECK_FALSE(matches("$artist = 'Bach' and 'x' = 'y'", bach));
    CHECK(matches("$genre = 'Jazz' and 'Jazz' = 'Jazz'", bach));
    CHECK_FALSE(matches("$genre = 'Jazz' and 'Jazz' = 'Jazz'", mozart));
  }

  TEST_CASE("PredicateOperandContext - sibling constants do not inherit a custom key", "[query][unit][predicate]")
  {
    auto labeled = TrackFixture{TrackSpec{.customPairs = {{"isrc", "ABC"}, {"label", "DG"}}}};
    auto other = TrackFixture{TrackSpec{.customPairs = {{"isrc", "XYZ"}}}};

    CHECK(matches("%isrc = 'ABC' and 'x' = 'x'", labeled));
    CHECK_FALSE(matches("%isrc = 'ABC' and 'x' = 'x'", other));
    CHECK_FALSE(matches("%isrc = 'ABC' and 'x' = 'y'", labeled));
    CHECK(matches("%isrc = 'ABC' and %label = 'DG'", labeled));
    CHECK_FALSE(matches("%isrc = 'ABC' and %label = 'DG'", other));
  }

  TEST_CASE("PredicateOperandContext - sibling constants do not inherit unit context", "[query][unit][predicate]")
  {
    auto matching = TrackFixture{TrackSpec{
      .duration = std::chrono::milliseconds{1500},
      .bitrate = 128000,
      .sampleRate = 44100,
    }};
    auto other = TrackFixture{TrackSpec{
      .duration = std::chrono::seconds{1},
      .bitrate = 256000,
      .sampleRate = 48000,
    }};

    CHECK(matches("@duration = 1.5s and 2 = 2", matching));
    CHECK_FALSE(matches("@duration = 1.5s and 2 = 2", other));
    CHECK_FALSE(matches("@duration = 1.5s and 2 = 3", matching));
    CHECK(matches("@sampleRate = 44.1k and 'x' = 'x'", matching));
    CHECK_FALSE(matches("@sampleRate = 44.1k and 'x' = 'x'", other));
    CHECK(matches("@bitrate = 128k and 1 = 1", matching));
    CHECK_FALSE(matches("@bitrate = 128k and 1 = 1", other));

    // A sibling unit literal keeps no numeric field, so it is not scaled by the preceding field.
    std::ignore = compileError(parseOk("@duration = 1.5s and 1s = 1000"));
    std::ignore = compileError(parseOk("@bitrate = 128k and 1k = 1000"));
    std::ignore = compileError(parseOk("@sampleRate = 44.1k and 44.1k = 44100"));
  }

  TEST_CASE("PredicateOperandContext - nested predicates compare as booleans", "[query][unit][predicate]")
  {
    auto flac = TrackFixture{TrackSpec{.title = "Goldberg", .codec = AudioCodec::Flac}};
    auto mp3 = TrackFixture{TrackSpec{.title = "Other", .codec = AudioCodec::Mp3}};
    auto bach = TrackFixture{TrackSpec{.artist = "Bach"}};
    auto mozart = TrackFixture{TrackSpec{.artist = "Mozart"}};
    auto timed = TrackFixture{TrackSpec{.duration = std::chrono::milliseconds{1500}}};
    auto shortTrack = TrackFixture{TrackSpec{.duration = std::chrono::seconds{1}}};
    auto labeled = TrackFixture{TrackSpec{.customPairs = {{"isrc", "ABC"}}}};
    auto unlabeled = TrackFixture{TrackSpec{}};

    CHECK(matches("(@codec = 'FLAC') = true", flac));
    CHECK_FALSE(matches("(@codec = 'FLAC') = true", mp3));
    CHECK(matches("(@codec = 'FLAC') = false", mp3));
    CHECK_FALSE(matches("(@codec = 'FLAC') = false", flac));
    CHECK(matches("($title = 'Goldberg') = true", flac));
    CHECK_FALSE(matches("($title = 'Goldberg') = true", mp3));
    CHECK(matches("($artist = 'Bach') = true", bach));
    CHECK_FALSE(matches("($artist = 'Bach') = true", mozart));
    CHECK(matches("(@duration = 1.5s) = true", timed));
    CHECK_FALSE(matches("(@duration = 1.5s) = true", shortTrack));
    CHECK(matches("(%isrc = 'ABC') = true", labeled));
    CHECK_FALSE(matches("(%isrc = 'ABC') = true", unlabeled));
    CHECK(matches("(%isrc = 'ABC') = false", unlabeled));
    CHECK_FALSE(matches("(%isrc = 'ABC') = false", labeled));
  }

  TEST_CASE("PredicateOperandContext - date and credit context stays on the direct field", "[query][unit][predicate]")
  {
    auto dated = TrackFixture{TrackSpec{
      .recordingDate = {.year = 1981, .month = 5, .day = 12},
      .credits = {{.name = "Glenn Gould", .kind = CreditKind::Performer, .role = "Piano"}},
    }};
    auto absent = TrackFixture{};

    CHECK(matches("$recordingDate = '1981-05-12' and 'x' = 'x'", dated));
    CHECK_FALSE(matches("$recordingDate = '1981-05-12' and 'x' = 'x'", absent));
    CHECK(matches("$recordingDate = 1981 and 'x' = 'x'", dated));
    CHECK_FALSE(matches("$recordingDate = '1981-05' and '1981' = '1981-05-12'", dated));
    CHECK(matches("($recordingDate = 1981) = true", dated));
    CHECK_FALSE(matches("($recordingDate = 1981) = true", absent));
    CHECK(matches("($recordingDate = 1981) = false", absent));
    CHECK_FALSE(matches("($recordingDate = 1981) = false", dated));

    CHECK(matches("$performer = 'Glenn Gould' and 'Piano' = 'Piano'", dated));
    CHECK_FALSE(matches("$performer = 'Piano' and 'x' = 'x'", dated));
    CHECK(matches("($credit = 'Glenn Gould') = true", dated));
    CHECK_FALSE(matches("($credit = 'Glenn Gould') = true", absent));
    CHECK(matches("($credit = 'Glenn Gould') = false", absent));
    CHECK_FALSE(matches("($credit = 'Glenn Gould') = false", dated));
  }

  TEST_CASE("PredicateOperandContext - every member selector confines nested and sibling field context",
            "[query][unit][predicate][credit]")
  {
    auto present = TrackFixture{TrackSpec{.credits = {
                                            {.name = "Name", .kind = CreditKind::Conductor},
                                            {.name = "Name", .kind = CreditKind::Ensemble},
                                            {.name = "Name", .kind = CreditKind::Soloist},
                                            {.name = "Name", .kind = CreditKind::Performer},
                                          }}};
    auto absent = TrackFixture{};

    for (auto const* field : {"conductor", "ensemble", "soloist", "performer", "credit"})
    {
      CHECK(matches(std::format("${} = 'Name' and 'x' = 'x'", field), present));
      CHECK_FALSE(matches(std::format("${} = 'Name' and 'x' = 'y'", field), present));
      CHECK(matches(std::format("${} = 'Name' and 'x' in ['y', 'x']", field), present));
      CHECK(matches(std::format("(${} = 'Name') = true", field), present));
      CHECK_FALSE(matches(std::format("(${} = 'Name') = true", field), absent));
      CHECK(matches(std::format("(${} = 'Name') = false", field), absent));
      CHECK(matches(std::format("(${}?) = true", field), present));
      CHECK_FALSE(matches(std::format("(${}?) = true", field), absent));
      CHECK(matches(std::format("(${} in ['Absent', 'Name']) = true", field), present));
      CHECK_FALSE(matches(std::format("(${} in ['Absent', 'Name']) = true", field), absent));
      CHECK(matches(std::format("(${} ~ 'NAME') = true", field), present));
      CHECK_FALSE(matches(std::format("(${} ~ 'NAME') = true", field), absent));
      CHECK(matches(std::format("(${0} = 'Name') = (${0}?)", field), present));
      CHECK(matches(std::format("(${0} = 'Name') = (${0}?)", field), absent));
    }
  }
} // namespace ao::query::test
