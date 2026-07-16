// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/query/Completion.h>
#include <ao/query/Expression.h>
#include <ao/query/Field.h>
#include <ao/query/FieldCatalog.h>
#include <ao/query/detail/FieldResolver.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
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
                       QueryVariableDescriptor const& descriptor,
                       QueryVariableCompletionMatchKind kind)
    {
      return std::ranges::any_of(matches,
                                 [&descriptor, kind](QueryVariableCompletionMatch const& match)
                                 {
                                   return match.type == descriptor.type && match.field == descriptor.field &&
                                          match.canonicalName == descriptor.canonicalName && match.kind == kind;
                                 });
    }
  } // namespace

  TEST_CASE("Completion - finds query variable tokens at the cursor", "[query][unit][completion]")
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

  TEST_CASE("Completion - rejects non-token cursor positions", "[query][unit][completion]")
  {
    CHECK_FALSE(queryCompletionTokenAtCursor("", 0));
    CHECK_FALSE(queryCompletionTokenAtCursor("$artist", 99));
    CHECK_FALSE(queryCompletionTokenAtCursor("$artist", 3));
    CHECK_FALSE(queryCompletionTokenAtCursor("foo$artist", 10));
    CHECK_FALSE(queryCompletionTokenAtCursor(R"("$artist")", 5));
    CHECK_FALSE(queryCompletionTokenAtCursor(R"(#"Rock")", 6));
    CHECK_FALSE(queryCompletionTokenAtCursor(R"(#["Rock"])", 7));
    CHECK_FALSE(analyzeQueryCompletion(R"($artist = "Mil)", 14));
  }

  TEST_CASE("Completion - keeps cursors inside tag tokens as variable completions", "[query][unit][completion]")
  {
    auto const text = std::string{"#rock"};
    auto const optContext = analyzeQueryCompletion(text, text.size());

    REQUIRE(optContext);
    auto const* token = std::get_if<QueryCompletionToken>(&*optContext);
    REQUIRE(token != nullptr);
    CHECK(token->type == VariableType::Tag);
    CHECK(token->trigger == '#');
    CHECK(token->replaceBegin == 0);
    CHECK(token->replaceEnd == text.size());
    CHECK(token->prefix == "rock");
  }

  TEST_CASE("Completion - preserves mid-input variable and malformed-tail analysis", "[query][unit][completion]")
  {
    SECTION("Variable before a suffix remains completable")
    {
      auto const text = std::string{"$ar + $album"};
      auto const cursor = text.find(' ');
      auto const optToken = queryCompletionTokenAtCursor(text, cursor);

      REQUIRE(optToken);
      CHECK(optToken->type == VariableType::Metadata);
      CHECK(optToken->replaceBegin == 0);
      CHECK(optToken->replaceEnd == cursor);
      CHECK(optToken->prefix == "ar");
    }

    SECTION("Malformed partial tail remains blocked before a suffix")
    {
      auto const text = std::string{R"(%"a\x"= + $artist)"};
      auto const cursor = text.find(" +");
      CHECK_FALSE(analyzeQueryCompletion(text, cursor));
    }
  }

  TEST_CASE("FieldCatalog - lists query variable descriptors in UI order", "[query][unit][field-catalog]")
  {
    auto const metadata = queryVariableDescriptors(VariableType::Metadata);
    REQUIRE(metadata.size() == 19);
    CHECK(metadata[0].canonicalName == "title");
    CHECK(metadata[1].canonicalName == "artist");
    CHECK(metadata[2].canonicalName == "album");
    CHECK(metadata[3].canonicalName == "albumArtist");
    CHECK(metadata[5].canonicalName == "conductor");
    CHECK(metadata[6].canonicalName == "ensemble");
    CHECK(metadata[8].canonicalName == "movement");
    CHECK(metadata[9].canonicalName == "soloist");
    CHECK(metadata[10].canonicalName == "genre");
    CHECK(metadata[16].canonicalName == "movementNumber");
    CHECK(metadata[18].canonicalName == "coverArt");

    auto const properties = queryVariableDescriptors(VariableType::Property);
    REQUIRE(properties.size() == 6);
    CHECK(properties[0].canonicalName == "duration");
    CHECK(properties[1].canonicalName == "bitrate");
    CHECK(properties[5].canonicalName == "codec");

    CHECK(queryVariableDescriptors(VariableType::Tag).empty());
    CHECK(queryVariableDescriptors(VariableType::Custom).empty());
  }

  TEST_CASE("Completion - completes canonical query variables", "[query][unit][completion]")
  {
    CHECK(canonicalNames(completeQueryVariable(VariableType::Metadata, "al")) ==
          std::vector<std::string_view>{"album", "albumArtist"});
    CHECK(canonicalNames(completeQueryVariable(VariableType::Property, "b")) ==
          std::vector<std::string_view>{"bitrate", "bitDepth"});
    CHECK(canonicalNames(completeQueryVariable(VariableType::Metadata, "COV")) ==
          std::vector<std::string_view>{"coverArt"});
  }

  TEST_CASE("Completion - expands exact aliases to canonical variables", "[query][unit][completion]")
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

  TEST_CASE("Completion - does not prefix match aliases", "[query][unit][completion]")
  {
    CHECK(completeQueryVariable(VariableType::Metadata, "t").size() >
          completeQueryVariable(VariableType::Metadata, "tn").size());
    CHECK(canonicalNames(completeQueryVariable(VariableType::Metadata, "d")) ==
          std::vector<std::string_view>{"discNumber", "discTotal"});
  }

  TEST_CASE("FieldCatalog - resolves every descriptor through parsing and completion", "[query][unit][field-catalog]")
  {
    for (auto const type : {VariableType::Metadata, VariableType::Property})
    {
      for (auto const& descriptor : queryVariableDescriptors(type))
      {
        DYNAMIC_SECTION("Canonical: " << descriptor.canonicalName)
        {
          auto const* found = findQueryVariableDescriptor(descriptor.type, descriptor.canonicalName);
          REQUIRE(found != nullptr);
          CHECK(found->field == descriptor.field);
          auto const field = detail::resolveVariableField(descriptor.type, descriptor.canonicalName);
          REQUIRE(field.has_value());
          CHECK(*field == descriptor.field);

          auto const matches = completeQueryVariable(descriptor.type, descriptor.canonicalName);
          CHECK(containsMatch(matches, descriptor, QueryVariableCompletionMatchKind::CanonicalPrefix));
        }

        for (auto const alias : descriptor.aliases)
        {
          DYNAMIC_SECTION("Alias: " << alias)
          {
            auto const* found = findQueryVariableDescriptor(descriptor.type, alias);
            REQUIRE(found != nullptr);
            CHECK(found->field == descriptor.field);
            auto const field = detail::resolveVariableField(descriptor.type, alias);
            REQUIRE(field.has_value());
            CHECK(*field == descriptor.field);

            auto const matches = completeQueryVariable(descriptor.type, alias);
            CHECK(containsMatch(matches, descriptor, QueryVariableCompletionMatchKind::ExactAlias));
          }
        }
      }
    }
  }

  TEST_CASE("Completion - returns nullopt for unknown variable fields", "[query][unit][completion]")
  {
    // Known fields resolve identically to the diagnostic detail::resolveVariableField().
    CHECK(detail::lookupVariableField(VariableType::Metadata, "artist") == Field::ArtistId);
    CHECK(detail::lookupVariableField(VariableType::Property, "duration") == Field::Duration);

    // Tag and Custom variables always resolve to their fixed fields.
    CHECK(detail::lookupVariableField(VariableType::Tag, "anything") == Field::Tag);
    CHECK(detail::lookupVariableField(VariableType::Custom, "anything") == Field::Custom);

    // Unknown property/metadata names degrade to nullopt instead of throwing.
    CHECK_FALSE(detail::lookupVariableField(VariableType::Metadata, "not_a_field").has_value());
    CHECK_FALSE(detail::lookupVariableField(VariableType::Property, "not_a_field").has_value());
  }

  TEST_CASE("Completion - documents query variable aliases in the catalog surface", "[query][unit][completion]")
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
    constexpr auto kM = std::array{std::string_view{"m"}};
    constexpr auto kG = std::array{std::string_view{"g"}};
    constexpr auto kY = std::array{std::string_view{"y"}};
    constexpr auto kTn = std::array{std::string_view{"tn"}};
    constexpr auto kTt = std::array{std::string_view{"tt"}};
    constexpr auto kDn = std::array{std::string_view{"dn"}};
    constexpr auto kTd = std::array{std::string_view{"td"}};
    constexpr auto kMn = std::array{std::string_view{"mn"}};
    constexpr auto kMt = std::array{std::string_view{"mt"}};
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
      Case{.type = VariableType::Metadata,
           .canonicalName = "conductor",
           .field = Field::ConductorId,
           .aliases = std::span{kNoAliases}},
      Case{.type = VariableType::Metadata,
           .canonicalName = "ensemble",
           .field = Field::EnsembleId,
           .aliases = std::span{kNoAliases}},
      Case{.type = VariableType::Metadata, .canonicalName = "work", .field = Field::WorkId, .aliases = std::span{kW}},
      Case{.type = VariableType::Metadata,
           .canonicalName = "movement",
           .field = Field::MovementId,
           .aliases = std::span{kM}},
      Case{.type = VariableType::Metadata,
           .canonicalName = "soloist",
           .field = Field::SoloistId,
           .aliases = std::span{kNoAliases}},
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
           .canonicalName = "movementNumber",
           .field = Field::MovementNumber,
           .aliases = std::span{kMn}},
      Case{.type = VariableType::Metadata,
           .canonicalName = "movementTotal",
           .field = Field::MovementTotal,
           .aliases = std::span{kMt}},
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

    auto const checkDescriptors = [&](std::span<QueryVariableDescriptor const> actual, auto const& expected)
    {
      REQUIRE(actual.size() == expected.size());

      for (std::size_t index = 0; auto const& descriptor : actual)
      {
        DYNAMIC_SECTION("Descriptor: " << descriptor.canonicalName)
        {
          auto const& exp = expected.at(index);
          CHECK(descriptor.type == exp.type);
          CHECK(descriptor.canonicalName == exp.canonicalName);
          CHECK(descriptor.field == exp.field);
          CHECK(std::ranges::equal(descriptor.aliases, exp.aliases));
        }

        ++index;
      }
    };

    checkDescriptors(queryVariableDescriptors(VariableType::Metadata), expectedMetadata);
    checkDescriptors(queryVariableDescriptors(VariableType::Property), expectedProperties);
  }

  TEST_CASE("FieldCatalog - finds descriptors by canonical name, alias, and field", "[query][unit][field-catalog]")
  {
    auto const* const canonical = findQueryVariableDescriptor(VariableType::Metadata, "movement");
    auto const* const alias = findQueryVariableDescriptor(VariableType::Metadata, "m");
    auto const* const byField = findQueryVariableDescriptor(Field::MovementId);

    REQUIRE(canonical != nullptr);
    CHECK(canonical->type == VariableType::Metadata);
    CHECK(canonical->field == Field::MovementId);
    REQUIRE(canonical->aliases.size() == 1);
    CHECK(canonical->aliases[0] == "m");
    CHECK(alias == canonical);
    CHECK(byField == canonical);

    CHECK(findQueryVariableDescriptor(VariableType::Metadata, "missing") == nullptr);
    CHECK(findQueryVariableDescriptor(Field::Uri) == nullptr);
  }

  TEST_CASE("FieldCatalog - supplies canonical diagnostic names and formatted variable text",
            "[query][unit][field-catalog]")
  {
    for (auto const type : {VariableType::Metadata, VariableType::Property})
    {
      for (auto const& descriptor : queryVariableDescriptors(type))
      {
        INFO("Field: " << descriptor.canonicalName);
        auto const variable =
          VariableExpression{.type = descriptor.type, .name = std::string{descriptor.canonicalName}};
        auto const expectedVariable =
          std::string(1, variablePrefix(descriptor.type)) + std::string{descriptor.canonicalName};

        CHECK(findQueryVariableDescriptor(descriptor.field) == &descriptor);
        CHECK(fieldDisplayName(descriptor.field) == descriptor.canonicalName);
        CHECK(variableDisplayName(descriptor.type, descriptor.canonicalName) == expectedVariable);
        CHECK(variableDisplayName(variable) == expectedVariable);
      }
    }

    CHECK(fieldDisplayName(Field::Uri) == "field");
    CHECK(variableDisplayName(VariableType::Tag, "90s Rock") == R"(#"90s Rock")");
    CHECK(variableDisplayName(VariableExpression{.type = VariableType::Tag, .name = "90s Rock"}) == R"(#"90s Rock")");
    CHECK(variableDisplayName(VariableType::Custom, R"(quote"key)") == R"(%"quote\"key")");
    CHECK(variableDisplayName(VariableExpression{.type = VariableType::Custom, .name = R"(quote"key)"}) ==
          R"(%"quote\"key")");
  }
} // namespace ao::query::test
