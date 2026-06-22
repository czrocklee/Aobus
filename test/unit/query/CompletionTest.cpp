// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/query/Completion.h>
#include <ao/query/Expression.h>
#include <ao/query/Field.h>
#include <ao/query/FieldCatalog.h>
#include <ao/query/Parser.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ao::query::test
{
  namespace
  {
    std::vector<std::string_view> canonicalNames(std::vector<QueryVariableCompletionMatch> const& matches)
    {
      auto names = std::vector<std::string_view>{};

      for (auto const& match : matches)
      {
        names.push_back(match.canonicalName);
      }

      return names;
    }

    bool containsMatch(std::span<QueryVariableCompletionMatch const> matches,
                       QueryVariableCompletionSpec const& spec,
                       QueryVariableCompletionMatchKind kind)
    {
      return std::ranges::any_of(matches,
                                 [&spec, kind](QueryVariableCompletionMatch const& match)
                                 {
                                   return match.type == spec.type && match.field == spec.field &&
                                          match.canonicalName == spec.canonicalName && match.kind == kind;
                                 });
    }

    bool isLogicalOperatorContext(std::optional<QueryCompletionContext> const& optContext)
    {
      return optContext && std::holds_alternative<QueryLogicalOperatorCompletionContext>(*optContext);
    }

    QueryOperatorCompletionContext operatorContext(std::optional<QueryCompletionContext> optContext)
    {
      REQUIRE(optContext);
      auto const* context = std::get_if<QueryOperatorCompletionContext>(&*optContext);
      REQUIRE(context != nullptr);
      return *context;
    }

    QueryValueCompletionContext valueContext(std::optional<QueryCompletionContext> optContext)
    {
      REQUIRE(optContext);
      auto const* context = std::get_if<QueryValueCompletionContext>(&*optContext);
      REQUIRE(context != nullptr);
      return *context;
    }

    QueryLogicalOperatorCompletionContext logicalOperatorContext(std::optional<QueryCompletionContext> optContext)
    {
      REQUIRE(optContext);
      auto const* context = std::get_if<QueryLogicalOperatorCompletionContext>(&*optContext);
      REQUIRE(context != nullptr);
      return *context;
    }
  } // namespace

  TEST_CASE("Completion - Finds Query Variable Token At Cursor", "[query][unit][completion]")
  {
    SECTION("Recognizes each trigger and returns replacement span")
    {
      auto const text = std::string{"$art @bit #rock %replay"};

      auto optMetadata = queryCompletionTokenAtCursor(text, 4);
      REQUIRE(optMetadata);
      CHECK(optMetadata->type == VariableType::Metadata);
      CHECK(optMetadata->trigger == '$');
      CHECK(optMetadata->replaceBegin == 0);
      CHECK(optMetadata->replaceEnd == 4);
      CHECK(optMetadata->prefix == "art");

      auto optProperty = queryCompletionTokenAtCursor(text, 9);
      REQUIRE(optProperty);
      CHECK(optProperty->type == VariableType::Property);
      CHECK(optProperty->trigger == '@');
      CHECK(optProperty->replaceBegin == 5);
      CHECK(optProperty->replaceEnd == 9);
      CHECK(optProperty->prefix == "bit");

      auto optTag = queryCompletionTokenAtCursor(text, 15);
      REQUIRE(optTag);
      CHECK(optTag->type == VariableType::Tag);
      CHECK(optTag->trigger == '#');
      CHECK(optTag->replaceBegin == 10);
      CHECK(optTag->replaceEnd == 15);
      CHECK(optTag->prefix == "rock");

      auto optCustom = queryCompletionTokenAtCursor(text, text.size());
      REQUIRE(optCustom);
      CHECK(optCustom->type == VariableType::Custom);
      CHECK(optCustom->trigger == '%');
      CHECK(optCustom->replaceBegin == 16);
      CHECK(optCustom->replaceEnd == text.size());
      CHECK(optCustom->prefix == "replay");
    }

    SECTION("Supports an empty prefix after a trigger")
    {
      auto optToken = queryCompletionTokenAtCursor("$", 1);
      REQUIRE(optToken);
      CHECK(optToken->type == VariableType::Metadata);
      CHECK(optToken->replaceBegin == 0);
      CHECK(optToken->replaceEnd == 1);
      CHECK(optToken->prefix.empty());
    }
  }

  TEST_CASE("Completion - Rejects Non Token Cursor Positions", "[query][unit][completion]")
  {
    CHECK_FALSE(queryCompletionTokenAtCursor("", 0));
    CHECK_FALSE(queryCompletionTokenAtCursor("$artist", 99));
    CHECK_FALSE(queryCompletionTokenAtCursor("$artist", 3));
    CHECK_FALSE(queryCompletionTokenAtCursor("foo$artist", 10));
    CHECK_FALSE(queryCompletionTokenAtCursor(R"("$artist")", 5));
    CHECK_FALSE(queryCompletionTokenAtCursor(R"(#"Rock")", 6));
    CHECK_FALSE(queryCompletionTokenAtCursor(R"(#["Rock"])", 7));
    CHECK_FALSE(analyzeCompletionContext(R"($artist = "Mil)", 14));
  }

  TEST_CASE("Completion - Keeps Cursor Inside Tag Token As Variable Completion", "[query][unit][completion]")
  {
    auto const text = std::string{"#rock"};
    auto const optContext = analyzeCompletionContext(text, text.size());

    REQUIRE(optContext);
    auto const* token = std::get_if<QueryCompletionToken>(&*optContext);
    REQUIRE(token != nullptr);
    CHECK(token->type == VariableType::Tag);
    CHECK(token->trigger == '#');
    CHECK(token->replaceBegin == 0);
    CHECK(token->replaceEnd == text.size());
    CHECK(token->prefix == "rock");
  }

  TEST_CASE("Completion - Lists Query Variable Specs In UI Catalog Order", "[query][unit][completion]")
  {
    auto const metadata = queryVariableCompletionSpecs(VariableType::Metadata);
    REQUIRE(metadata.size() == 13);
    CHECK(metadata[0].canonicalName == "title");
    CHECK(metadata[1].canonicalName == "artist");
    CHECK(metadata[2].canonicalName == "album");
    CHECK(metadata[3].canonicalName == "albumArtist");
    CHECK(metadata[12].canonicalName == "coverArt");

    auto const properties = queryVariableCompletionSpecs(VariableType::Property);
    REQUIRE(properties.size() == 6);
    CHECK(properties[0].canonicalName == "duration");
    CHECK(properties[1].canonicalName == "bitrate");
    CHECK(properties[5].canonicalName == "codec");

    CHECK(queryVariableCompletionSpecs(VariableType::Tag).empty());
    CHECK(queryVariableCompletionSpecs(VariableType::Custom).empty());
  }

  TEST_CASE("Completion - Completes Canonical Query Variables", "[query][unit][completion]")
  {
    CHECK(canonicalNames(completeQueryVariable(VariableType::Metadata, "al")) ==
          std::vector<std::string_view>{"album", "albumArtist"});
    CHECK(canonicalNames(completeQueryVariable(VariableType::Property, "b")) ==
          std::vector<std::string_view>{"bitrate", "bitDepth"});
    CHECK(canonicalNames(completeQueryVariable(VariableType::Metadata, "COV")) ==
          std::vector<std::string_view>{"coverArt"});
  }

  TEST_CASE("Completion - Expands Exact Aliases To Canonical Variables", "[query][unit][completion]")
  {
    auto albumMatches = completeQueryVariable(VariableType::Metadata, "al");
    REQUIRE(albumMatches.size() == 2);
    CHECK(albumMatches[0].canonicalName == "album");
    CHECK(albumMatches[0].kind == QueryVariableCompletionMatchKind::ExactAlias);
    CHECK(albumMatches[1].canonicalName == "albumArtist");
    CHECK(albumMatches[1].kind == QueryVariableCompletionMatchKind::CanonicalPrefix);

    auto trackNumberMatches = completeQueryVariable(VariableType::Metadata, "tn");
    REQUIRE(trackNumberMatches.size() == 1);
    CHECK(trackNumberMatches[0].canonicalName == "trackNumber");
    CHECK(trackNumberMatches[0].field == Field::TrackNumber);
    CHECK(trackNumberMatches[0].kind == QueryVariableCompletionMatchKind::ExactAlias);

    auto bitrateMatches = completeQueryVariable(VariableType::Property, "BR");
    REQUIRE(bitrateMatches.size() == 1);
    CHECK(bitrateMatches[0].canonicalName == "bitrate");
    CHECK(bitrateMatches[0].kind == QueryVariableCompletionMatchKind::ExactAlias);
  }

  TEST_CASE("Completion - Does Not Prefix Match Aliases", "[query][unit][completion]")
  {
    CHECK(completeQueryVariable(VariableType::Metadata, "t").size() >
          completeQueryVariable(VariableType::Metadata, "tn").size());
    CHECK(canonicalNames(completeQueryVariable(VariableType::Metadata, "d")) ==
          std::vector<std::string_view>{"discNumber", "discTotal"});
  }

  TEST_CASE("Completion - Query Variable Specs Resolve Through Catalog", "[query][unit][completion]")
  {
    for (auto const type : {VariableType::Metadata, VariableType::Property})
    {
      for (auto const& spec : queryVariableCompletionSpecs(type))
      {
        DYNAMIC_SECTION("Canonical: " << spec.canonicalName)
        {
          auto const* found = findQueryVariableCompletionSpec(spec.type, spec.canonicalName);
          REQUIRE(found != nullptr);
          CHECK(found->field == spec.field);
          auto const field = resolveVariableField(spec.type, spec.canonicalName);
          REQUIRE(field.has_value());
          CHECK(*field == spec.field);

          auto const matches = completeQueryVariable(spec.type, spec.canonicalName);
          CHECK(containsMatch(matches, spec, QueryVariableCompletionMatchKind::CanonicalPrefix));
        }

        for (auto const alias : spec.aliases)
        {
          DYNAMIC_SECTION("Alias: " << alias)
          {
            auto const* found = findQueryVariableCompletionSpec(spec.type, alias);
            REQUIRE(found != nullptr);
            CHECK(found->field == spec.field);
            auto const field = resolveVariableField(spec.type, alias);
            REQUIRE(field.has_value());
            CHECK(*field == spec.field);

            auto const matches = completeQueryVariable(spec.type, alias);
            CHECK(containsMatch(matches, spec, QueryVariableCompletionMatchKind::ExactAlias));
          }
        }
      }
    }
  }

  TEST_CASE("Completion - tryResolveVariableField Returns nullopt For Unknown Fields", "[query][unit][completion]")
  {
    // Known fields resolve identically to the diagnostic resolveVariableField().
    CHECK(tryResolveVariableField(VariableType::Metadata, "artist") == Field::ArtistId);
    CHECK(tryResolveVariableField(VariableType::Property, "duration") == Field::Duration);

    // Tag and Custom variables always resolve to their fixed fields.
    CHECK(tryResolveVariableField(VariableType::Tag, "anything") == Field::Tag);
    CHECK(tryResolveVariableField(VariableType::Custom, "anything") == Field::Custom);

    // Unknown property/metadata names degrade to nullopt instead of throwing.
    CHECK_FALSE(tryResolveVariableField(VariableType::Metadata, "not_a_field").has_value());
    CHECK_FALSE(tryResolveVariableField(VariableType::Property, "not_a_field").has_value());
  }

  TEST_CASE("Completion - Query Variable Alias Set Documents Catalog Surface", "[query][unit][completion]")
  {
    struct Case final
    {
      VariableType type = VariableType::Metadata;
      std::string_view canonicalName;
      Field field = Field::Title;
      std::span<std::string_view const> aliases;
    };

    constexpr auto kNoAliases = std::array<std::string_view, 0>{};
    constexpr auto kT = std::array{std::string_view{"t"}};
    constexpr auto kA = std::array{std::string_view{"a"}};
    constexpr auto kAl = std::array{std::string_view{"al"}};
    constexpr auto kAa = std::array{std::string_view{"aa"}};
    constexpr auto kC = std::array{std::string_view{"c"}};
    constexpr auto kW = std::array{std::string_view{"w"}};
    constexpr auto kG = std::array{std::string_view{"g"}};
    constexpr auto kY = std::array{std::string_view{"y"}};
    constexpr auto kTn = std::array{std::string_view{"tn"}};
    constexpr auto kTt = std::array{std::string_view{"tt"}};
    constexpr auto kDn = std::array{std::string_view{"dn"}};
    constexpr auto kTd = std::array{std::string_view{"td"}};
    constexpr auto kCa = std::array{std::string_view{"ca"}};
    constexpr auto kL = std::array{std::string_view{"l"}};
    constexpr auto kBr = std::array{std::string_view{"br"}};
    constexpr auto kSr = std::array{std::string_view{"sr"}};
    constexpr auto kBd = std::array{std::string_view{"bd"}};

    // This intentionally locks the public catalog surface; update these expectations with catalog changes.
    auto const expectedMetadata = std::array{
      Case{.type = VariableType::Metadata, .canonicalName = "title", .field = Field::Title, .aliases = std::span{kT}},
      Case{
        .type = VariableType::Metadata, .canonicalName = "artist", .field = Field::ArtistId, .aliases = std::span{kA}},
      Case{
        .type = VariableType::Metadata, .canonicalName = "album", .field = Field::AlbumId, .aliases = std::span{kAl}},
      Case{.type = VariableType::Metadata,
           .canonicalName = "albumArtist",
           .field = Field::AlbumArtistId,
           .aliases = std::span{kAa}},
      Case{.type = VariableType::Metadata,
           .canonicalName = "composer",
           .field = Field::ComposerId,
           .aliases = std::span{kC}},
      Case{.type = VariableType::Metadata, .canonicalName = "work", .field = Field::WorkId, .aliases = std::span{kW}},
      Case{.type = VariableType::Metadata, .canonicalName = "genre", .field = Field::GenreId, .aliases = std::span{kG}},
      Case{.type = VariableType::Metadata, .canonicalName = "year", .field = Field::Year, .aliases = std::span{kY}},
      Case{.type = VariableType::Metadata,
           .canonicalName = "trackNumber",
           .field = Field::TrackNumber,
           .aliases = std::span{kTn}},
      Case{.type = VariableType::Metadata,
           .canonicalName = "trackTotal",
           .field = Field::TrackTotal,
           .aliases = std::span{kTt}},
      Case{.type = VariableType::Metadata,
           .canonicalName = "discNumber",
           .field = Field::DiscNumber,
           .aliases = std::span{kDn}},
      Case{.type = VariableType::Metadata,
           .canonicalName = "discTotal",
           .field = Field::DiscTotal,
           .aliases = std::span{kTd}},
      Case{.type = VariableType::Metadata,
           .canonicalName = "coverArt",
           .field = Field::CoverArtId,
           .aliases = std::span{kCa}},
    };
    auto const expectedProperties = std::array{
      Case{.type = VariableType::Property,
           .canonicalName = "duration",
           .field = Field::Duration,
           .aliases = std::span{kL}},
      Case{
        .type = VariableType::Property, .canonicalName = "bitrate", .field = Field::Bitrate, .aliases = std::span{kBr}},
      Case{.type = VariableType::Property,
           .canonicalName = "sampleRate",
           .field = Field::SampleRate,
           .aliases = std::span{kSr}},
      Case{.type = VariableType::Property,
           .canonicalName = "channels",
           .field = Field::Channels,
           .aliases = std::span{kNoAliases}},
      Case{.type = VariableType::Property,
           .canonicalName = "bitDepth",
           .field = Field::BitDepth,
           .aliases = std::span{kBd}},
      Case{.type = VariableType::Property,
           .canonicalName = "codec",
           .field = Field::Codec,
           .aliases = std::span{kNoAliases}},
    };

    auto const assertSpecs = [&](std::span<QueryVariableCompletionSpec const> actual, auto const& expected)
    {
      REQUIRE(actual.size() == expected.size());

      for (std::size_t idx = 0; auto const& spec : actual)
      {
        DYNAMIC_SECTION("Spec: " << spec.canonicalName)
        {
          auto const& exp = expected.at(idx);
          CHECK(spec.type == exp.type);
          CHECK(spec.canonicalName == exp.canonicalName);
          CHECK(spec.field == exp.field);
          CHECK(std::ranges::equal(spec.aliases, exp.aliases));
        }

        ++idx;
      }
    };

    assertSpecs(queryVariableCompletionSpecs(VariableType::Metadata), expectedMetadata);
    assertSpecs(queryVariableCompletionSpecs(VariableType::Property), expectedProperties);
  }

  TEST_CASE("Completion - Analyzes Operator Context After Query Variables", "[query][unit][completion]")
  {
    SECTION("Offers an empty operator prefix after trailing whitespace")
    {
      auto const text = std::string{"$artist "};
      auto const context = operatorContext(analyzeCompletionContext(text, text.size()));

      CHECK(context.field == Field::ArtistId);
      CHECK(context.replacement.replaceBegin == 7);
      CHECK(context.replacement.replaceEnd == text.size());
      CHECK(context.replacement.prefix.empty());
      CHECK(completeQueryOperator(context.field, context.replacement.prefix) ==
            std::vector<std::string_view>{"=", "!=", "~", "in", "?"});
    }

    SECTION("Normalizes replacement over whitespace and typed symbol prefixes")
    {
      auto const text = std::string{"$year >"};
      auto const context = operatorContext(analyzeCompletionContext(text, text.size()));

      CHECK(context.field == Field::Year);
      CHECK(context.replacement.replaceBegin == 5);
      CHECK(context.replacement.replaceEnd == text.size());
      CHECK(context.replacement.prefix == ">");
      CHECK(completeQueryOperator(context.field, context.replacement.prefix) ==
            std::vector<std::string_view>{">", ">="});
    }

    SECTION("Completes keyword operators only after a variable boundary")
    {
      auto const text = std::string{"$artist i"};
      auto const context = operatorContext(analyzeCompletionContext(text, text.size()));

      CHECK(context.field == Field::ArtistId);
      CHECK(context.replacement.replaceBegin == 7);
      CHECK(context.replacement.replaceEnd == text.size());
      CHECK(context.replacement.prefix == "i");
      CHECK(completeQueryOperator(context.field, context.replacement.prefix) == std::vector<std::string_view>{"in"});

      auto optGlued = queryCompletionTokenAtCursor("$artistin", 9);
      REQUIRE(optGlued);
      CHECK(optGlued->prefix == "artistin");
      CHECK(completeQueryVariable(optGlued->type, optGlued->prefix).empty());
    }

    SECTION("Resolves quoted user variables as lvalues")
    {
      auto const text = std::string{R"(%"Mood" )"};
      auto const context = operatorContext(analyzeCompletionContext(text, text.size()));

      CHECK(context.field == Field::Custom);
      CHECK(context.replacement.replaceBegin == 7);
      CHECK(context.replacement.replaceEnd == text.size());
      CHECK(completeQueryOperator(context.field, context.replacement.prefix) ==
            std::vector<std::string_view>{"=", "!=", "~", "in", "?"});
    }

    SECTION("Resolves bracketed quoted user variables as lvalues")
    {
      auto const text = std::string{R"(%["Replay Gain"] )"};
      auto const context = operatorContext(analyzeCompletionContext(text, text.size()));

      CHECK(context.field == Field::Custom);
      CHECK(context.replacement.replaceBegin == text.size() - 1);
      CHECK(context.replacement.replaceEnd == text.size());
      CHECK(completeQueryOperator(context.field, context.replacement.prefix) ==
            std::vector<std::string_view>{"=", "!=", "~", "in", "?"});
    }

    SECTION("Resolves bracketed quoted user variables as lvalues without trailing space")
    {
      auto const text = std::string{R"(%["Replay Gain"])"};
      auto const context = operatorContext(analyzeCompletionContext(text, text.size()));

      CHECK(context.field == Field::Custom);
      CHECK(context.replacement.replaceBegin == text.size());
      CHECK(context.replacement.replaceEnd == text.size());
      CHECK(completeQueryOperator(context.field, context.replacement.prefix) ==
            std::vector<std::string_view>{"=", "!=", "~", "in", "?"});
    }

    SECTION("Rejects invalid escape in quoted variable as operator lvalue")
    {
      auto const text = std::string{R"(%"a\x"=)"};
      CHECK_FALSE(analyzeCompletionContext(text, text.size()));
    }
  }

  TEST_CASE("Completion - Filters Operator Catalog By Field Kind", "[query][unit][completion]")
  {
    CHECK(completeQueryOperator(Field::ArtistId, "") == std::vector<std::string_view>{"=", "!=", "~", "in", "?"});
    CHECK(completeQueryOperator(Field::Year, "") ==
          std::vector<std::string_view>{"=", "!=", "<", "<=", ">", ">=", "in", "?"});
    CHECK(completeQueryOperator(Field::Tag, "") == std::vector<std::string_view>{"?"});
    CHECK(completeQueryOperator(Field::ArtistId, "!") == std::vector<std::string_view>{"!="});
  }

  TEST_CASE("Completion - Analyzes Logical Operator Context After Complete Expressions", "[query][unit][completion]")
  {
    SECTION("Offers an empty logical operator prefix after a comparison")
    {
      auto const text = std::string{R"($artist = "Miles" )"};
      auto const context = logicalOperatorContext(analyzeCompletionContext(text, text.size()));

      CHECK(context.replacement.replaceBegin == text.find_last_of('"') + 1);
      CHECK(context.replacement.replaceEnd == text.size());
      CHECK(context.replacement.prefix.empty());
      CHECK(completeQueryLogicalOperator(context.replacement.prefix) ==
            std::vector<std::string_view>{"and", "or", "&&", "||"});
    }

    SECTION("Completes keyword logical operators after a typed prefix")
    {
      auto const text = std::string{R"($artist = "Miles" a)"};
      auto const context = logicalOperatorContext(analyzeCompletionContext(text, text.size()));

      CHECK(context.replacement.replaceBegin == text.find_last_of('"') + 1);
      CHECK(context.replacement.replaceEnd == text.size());
      CHECK(context.replacement.prefix == "a");
      CHECK(completeQueryLogicalOperator(context.replacement.prefix) == std::vector<std::string_view>{"and"});
    }

    SECTION("Completes symbolic logical operators after a typed prefix")
    {
      auto const text = std::string{R"($year >= 1999 &)"};
      auto const context = logicalOperatorContext(analyzeCompletionContext(text, text.size()));

      CHECK(context.replacement.replaceBegin == text.find("1999") + 4);
      CHECK(context.replacement.replaceEnd == text.size());
      CHECK(context.replacement.prefix == "&");
      CHECK(completeQueryLogicalOperator(context.replacement.prefix) == std::vector<std::string_view>{"&&"});
    }

    SECTION("Treats postfix exists as a complete expression")
    {
      auto const text = std::string{"$artist?"};
      auto const context = logicalOperatorContext(analyzeCompletionContext(text, text.size()));

      CHECK(context.replacement.replaceBegin == text.size());
      CHECK(context.replacement.replaceEnd == text.size());
      CHECK(context.replacement.prefix.empty());
    }

    SECTION("Treats bare tag variables as complete expressions")
    {
      auto const cases =
        std::array{std::string{"#rock "}, std::string{R"(#"90s Rock" )"}, std::string{R"(#["90s Rock"] )"}};

      for (auto const& text : cases)
      {
        DYNAMIC_SECTION("Text: " << text)
        {
          auto const context = logicalOperatorContext(analyzeCompletionContext(text, text.size()));

          CHECK(context.replacement.replaceBegin == text.size() - 1);
          CHECK(context.replacement.replaceEnd == text.size());
          CHECK(context.replacement.prefix.empty());
          CHECK(completeQueryLogicalOperator(context.replacement.prefix) ==
                std::vector<std::string_view>{"and", "or", "&&", "||"});
        }
      }
    }

    SECTION("Completes logical operator prefixes after bare tags")
    {
      auto const text = std::string{"#rock o"};
      auto const context = logicalOperatorContext(analyzeCompletionContext(text, text.size()));

      CHECK(context.replacement.replaceBegin == text.find(' '));
      CHECK(context.replacement.replaceEnd == text.size());
      CHECK(context.replacement.prefix == "o");
      CHECK(completeQueryLogicalOperator(context.replacement.prefix) == std::vector<std::string_view>{"or"});
    }

    SECTION("Does not offer logical operators after an incomplete comparison")
    {
      CHECK_FALSE(analyzeCompletionContext("$artist = a", 10));
    }

    SECTION("Completes symbolic logical operator from an Unknown ampersand tail")
    {
      auto const text = std::string{"$year >= 1999 &"};
      auto const context = logicalOperatorContext(analyzeCompletionContext(text, text.size()));

      CHECK(context.replacement.replaceBegin == text.find_last_of(' '));
      CHECK(context.replacement.replaceEnd == text.size());
      CHECK(context.replacement.prefix == "&");
      CHECK(completeQueryLogicalOperator(context.replacement.prefix) == std::vector<std::string_view>{"&&"});
    }
  }

  TEST_CASE("Completion - Backward Scanner Tracks Parser Complete Predicate Boundaries", "[query][unit][completion]")
  {
    auto const assertCompletePredicateBoundary = [](std::string_view expression)
    {
      REQUIRE(parse(expression).has_value());

      auto const text = std::string{expression} + " ";
      auto const context = logicalOperatorContext(analyzeCompletionContext(text, text.size()));

      CHECK(context.replacement.replaceBegin == expression.size());
      CHECK(context.replacement.replaceEnd == text.size());
      CHECK(context.replacement.prefix.empty());
    };

    auto const completePredicates = std::array{
      std::string_view{R"($artist = "Miles")"},
      std::string_view{R"($artist != "Miles")"},
      std::string_view{"$title ~ Love"},
      std::string_view{"$year < 2000"},
      std::string_view{"$year <= 2000"},
      std::string_view{"$year > 1999"},
      std::string_view{"$year >= 1999"},
      std::string_view{R"($artist in ["Miles", "Monk"])"},
      std::string_view{"$year in 1990..1999"},
      std::string_view{"@duration in 2m30s..5m"},
      std::string_view{"$artist?"},
      std::string_view{R"(%["Replay Gain"]?)"},
      std::string_view{"#rock"},
      std::string_view{R"(#"90s Rock")"},
      std::string_view{R"(#["90s Rock"])"},
      std::string_view{R"(($artist = "Miles"))"},
      std::string_view{R"(($artist in ["Miles", "Monk"]))"},
      std::string_view{R"((($artist = "Miles") and ($album = "Kind of Blue")))"},
    };

    for (auto const expression : completePredicates)
    {
      DYNAMIC_SECTION("Expression: " << expression)
      {
        assertCompletePredicateBoundary(expression);
      }
    }
  }

  TEST_CASE("Completion - Backward Scanner Does Not Promote Parser Incomplete Expressions", "[query][unit][completion]")
  {
    auto const incompleteExpressions = std::array{
      std::string_view{"$artist ="},
      std::string_view{"$artist !="},
      std::string_view{"$title ~"},
      std::string_view{"$year <"},
      std::string_view{"$year <="},
      std::string_view{"$year >"},
      std::string_view{"$year >="},
      std::string_view{"$artist in"},
      std::string_view{"$artist in ["},
      std::string_view{R"($artist in ["Miles",)"},
      std::string_view{"$year in 1990.."},
      std::string_view{"@duration in 2m.."},
      std::string_view{R"(#"unterminated)"},
      std::string_view{R"(%["Replay Gain"?)"},
      std::string_view{R"(($artist = "Miles")"},
    };

    for (auto const expression : incompleteExpressions)
    {
      DYNAMIC_SECTION("Expression: " << expression)
      {
        REQUIRE_FALSE(parse(expression).has_value());

        auto const text = std::string{expression} + " ";
        CHECK_FALSE(isLogicalOperatorContext(analyzeCompletionContext(text, text.size())));
      }
    }
  }

  TEST_CASE("Completion - Backward Scanner Does Not Promote Non-Predicate Expressions", "[query][unit][completion]")
  {
    auto const nonPredicateExpressions = std::array{
      std::string_view{"$title"},
      std::string_view{"$artist"},
      std::string_view{"@duration"},
      std::string_view{"%rating"},
      std::string_view{"3m"},
      std::string_view{R"("literal")"},
      std::string_view{R"($title + " - " + $artist)"},
    };

    for (auto const expression : nonPredicateExpressions)
    {
      DYNAMIC_SECTION("Expression: " << expression)
      {
        REQUIRE(parse(expression).has_value());

        auto const text = std::string{expression} + " ";
        CHECK_FALSE(isLogicalOperatorContext(analyzeCompletionContext(text, text.size())));
      }
    }
  }

  TEST_CASE("Completion - Backward Scanner Rejects Parser Invalid Group Boundaries", "[query][unit][completion]")
  {
    auto const invalidGroupedExpressions = std::array{
      std::string_view{"($artist =)"},
      std::string_view{"($year >)"},
      std::string_view{"(garbage +)"},
      std::string_view{R"(($artist in ["Miles",]))"},
    };

    for (auto const expression : invalidGroupedExpressions)
    {
      DYNAMIC_SECTION("Expression: " << expression)
      {
        REQUIRE_FALSE(parse(expression).has_value());

        auto const text = std::string{expression} + " ";
        CHECK_FALSE(analyzeCompletionContext(text, text.size()));
      }
    }
  }

  TEST_CASE("Completion - Analyzes Value Context After Complete Operators", "[query][unit][completion]")
  {
    SECTION("Detects a normal comparison value")
    {
      auto const text = std::string{"$artist = Ma"};
      auto const context = valueContext(analyzeCompletionContext(text, text.size()));

      CHECK(context.field == Field::ArtistId);
      CHECK(context.replacement.replaceBegin == text.find("Ma"));
      CHECK(context.replacement.replaceEnd == text.size());
      CHECK(context.replacement.prefix == "Ma");
    }

    SECTION("Detects an empty comparison value")
    {
      auto const text = std::string{"$artist ="};
      auto const context = valueContext(analyzeCompletionContext(text, text.size()));

      CHECK(context.field == Field::ArtistId);
      CHECK(context.replacement.replaceBegin == text.size());
      CHECK(context.replacement.replaceEnd == text.size());
      CHECK(context.replacement.prefix.empty());
    }

    SECTION("Keeps in-list values bound to the left field")
    {
      auto const text = std::string{"$artist in [Ma"};
      auto const context = valueContext(analyzeCompletionContext(text, text.size()));

      CHECK(context.field == Field::ArtistId);
      CHECK(context.replacement.replaceBegin == text.find("Ma"));
      CHECK(context.replacement.replaceEnd == text.size());
      CHECK(context.replacement.prefix == "Ma");
    }

    SECTION("Handles later values in an in-list")
    {
      auto const text = std::string{R"($artist in ["Miles", Mo)"};
      auto const context = valueContext(analyzeCompletionContext(text, text.size()));

      CHECK(context.field == Field::ArtistId);
      CHECK(context.replacement.replaceBegin == text.find("Mo"));
      CHECK(context.replacement.replaceEnd == text.size());
      CHECK(context.replacement.prefix == "Mo");
    }

    SECTION("Handles quoted custom keys before values")
    {
      auto const text = std::string{R"(%"Mood" = Br)"};
      auto const context = valueContext(analyzeCompletionContext(text, text.size()));

      CHECK(context.field == Field::Custom);
      CHECK(context.replacement.replaceBegin == text.find("Br"));
      CHECK(context.replacement.replaceEnd == text.size());
      CHECK(context.replacement.prefix == "Br");
    }

    SECTION("Ignores escaped quotes inside custom keys")
    {
      auto const text = std::string{R"(%"quote\"key" = Br)"};
      auto const context = valueContext(analyzeCompletionContext(text, text.size()));

      CHECK(context.field == Field::Custom);
      CHECK(context.replacement.replaceBegin == text.find("Br"));
      CHECK(context.replacement.replaceEnd == text.size());
      CHECK(context.replacement.prefix == "Br");
    }
  }
} // namespace ao::query::test
