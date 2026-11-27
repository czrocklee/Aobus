// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "lib/query/detail/FieldResolver.h"
#include <ao/query/Completion.h>
#include <ao/query/Expression.h>
#include <ao/query/Field.h>
#include <ao/query/FieldCatalog.h>

#include <catch2/catch_message.hpp>
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

    std::optional<QueryVariableCompletion> variableCompletionAtCursor(std::string_view text, std::size_t cursor)
    {
      auto optAnalysis = analyzeQueryCompletion(text, cursor);

      if (!optAnalysis)
      {
        return std::nullopt;
      }

      if (auto const* token = std::get_if<QueryVariableCompletion>(&*optAnalysis); token != nullptr)
      {
        return *token;
      }

      return std::nullopt;
    }
  } // namespace

  TEST_CASE("Completion - finds query variable tokens at the cursor")
  {
    SECTION("Recognizes each trigger and returns replacement span")
    {
      auto const text = std::string{"$art @bit #rock %replay"};

      auto optMetadata = variableCompletionAtCursor(text, 4);
      REQUIRE(optMetadata);
      CHECK(optMetadata->type == VariableType::Metadata);
      CHECK(optMetadata->replacement.replaceBegin == 0);
      CHECK(optMetadata->replacement.replaceEnd == 4);
      CHECK(optMetadata->replacement.prefix == "art");

      auto optProperty = variableCompletionAtCursor(text, 9);
      REQUIRE(optProperty);
      CHECK(optProperty->type == VariableType::Property);
      CHECK(optProperty->replacement.replaceBegin == 5);
      CHECK(optProperty->replacement.replaceEnd == 9);
      CHECK(optProperty->replacement.prefix == "bit");

      auto optTag = variableCompletionAtCursor(text, 15);
      REQUIRE(optTag);
      CHECK(optTag->type == VariableType::Tag);
      CHECK(optTag->replacement.replaceBegin == 10);
      CHECK(optTag->replacement.replaceEnd == 15);
      CHECK(optTag->replacement.prefix == "rock");

      auto optCustom = variableCompletionAtCursor(text, text.size());
      REQUIRE(optCustom);
      CHECK(optCustom->type == VariableType::Custom);
      CHECK(optCustom->replacement.replaceBegin == 16);
      CHECK(optCustom->replacement.replaceEnd == text.size());
      CHECK(optCustom->replacement.prefix == "replay");
    }

    SECTION("Supports an empty prefix after a trigger")
    {
      auto optToken = variableCompletionAtCursor("$", 1);
      REQUIRE(optToken);
      CHECK(optToken->type == VariableType::Metadata);
      CHECK(optToken->replacement.replaceBegin == 0);
      CHECK(optToken->replacement.replaceEnd == 1);
      CHECK(optToken->replacement.prefix.empty());
    }
  }

  TEST_CASE("Completion - rejects non-token cursor positions")
  {
    CHECK_FALSE(variableCompletionAtCursor("", 0));
    CHECK_FALSE(variableCompletionAtCursor("$artist", 99));
    CHECK_FALSE(variableCompletionAtCursor("$artist", 3));

    constexpr auto kGluedVariables = std::array{
      std::string_view{"foo$artist"},
      std::string_view{"foo@bitrate"},
      std::string_view{"foo#rock"},
      std::string_view{"foo%rating"},
    };

    for (auto const text : kGluedVariables)
    {
      DYNAMIC_SECTION("Text: " << text)
      {
        CHECK_FALSE(variableCompletionAtCursor(text, text.size()));
      }
    }

    CHECK_FALSE(variableCompletionAtCursor(R"("$artist")", 5));
    CHECK_FALSE(variableCompletionAtCursor(R"(#"Rock")", 6));
    CHECK_FALSE(variableCompletionAtCursor(R"(#["Rock"])", 7));
    CHECK_FALSE(analyzeQueryCompletion(R"($artist = "Mil)", 14));
  }

  TEST_CASE("Completion - prefers a variable completion at the end of a tag")
  {
    auto const text = std::string{"#rock"};
    auto const optContext = analyzeQueryCompletion(text, text.size());

    REQUIRE(optContext);
    auto const* token = std::get_if<QueryVariableCompletion>(&*optContext);
    REQUIRE(token != nullptr);
    CHECK(token->type == VariableType::Tag);
    CHECK(token->replacement.replaceBegin == 0);
    CHECK(token->replacement.replaceEnd == text.size());
    CHECK(token->replacement.prefix == "rock");
  }

  TEST_CASE("Completion - preserves mid-input variable and malformed-tail analysis")
  {
    SECTION("Variable before a suffix remains completable")
    {
      auto const text = std::string{"$ar + $album"};
      auto const cursor = text.find(' ');
      auto const optToken = variableCompletionAtCursor(text, cursor);

      REQUIRE(optToken);
      CHECK(optToken->type == VariableType::Metadata);
      CHECK(optToken->replacement.replaceBegin == 0);
      CHECK(optToken->replacement.replaceEnd == cursor);
      CHECK(optToken->replacement.prefix == "ar");
    }

    SECTION("Variable after a logical operator replaces only its own text")
    {
      constexpr auto kText = std::string_view{R"($album = "Kind" and $ye)"};
      auto const optContext = variableCompletionAtCursor(kText, kText.size());
      REQUIRE(optContext);
      CHECK(optContext->replacement.replaceBegin == 20);
      CHECK(optContext->replacement.replaceEnd == kText.size());
      CHECK(optContext->replacement.prefix == "ye");
      CHECK(canonicalNames(completeQueryVariable(optContext->type, optContext->replacement.prefix)) ==
            std::vector<std::string_view>{"year"});
    }

    SECTION("Malformed partial tail remains blocked before a suffix")
    {
      auto const text = std::string{R"(%"a\x"= + $artist)"};
      auto const cursor = text.find(" +");
      CHECK_FALSE(analyzeQueryCompletion(text, cursor));
    }
  }

  TEST_CASE("FieldCatalog - tag and custom names do not come from the system catalog")
  {
    CHECK(queryVariableDescriptors(VariableType::Tag).empty());
    CHECK(queryVariableDescriptors(VariableType::Custom).empty());
  }

  TEST_CASE("Completion - completes canonical query variables")
  {
    CHECK(canonicalNames(completeQueryVariable(VariableType::Metadata, "")) ==
          std::vector<std::string_view>{"title",         "artist",     "album",      "albumArtist", "composer",
                                        "conductor",     "ensemble",   "work",       "movement",    "soloist",
                                        "recordingDate", "performer",  "credit",     "genre",       "year",
                                        "trackNumber",   "trackTotal", "discNumber", "discTotal",   "movementNumber",
                                        "movementTotal", "coverArt"});
    CHECK(canonicalNames(completeQueryVariable(VariableType::Property, "")) ==
          std::vector<std::string_view>{"duration", "bitrate", "sampleRate", "channels", "bitDepth", "codec"});
    CHECK(canonicalNames(completeQueryVariable(VariableType::Metadata, "al")) ==
          std::vector<std::string_view>{"album", "albumArtist"});
    CHECK(canonicalNames(completeQueryVariable(VariableType::Property, "b")) ==
          std::vector<std::string_view>{"bitrate", "bitDepth"});
    CHECK(canonicalNames(completeQueryVariable(VariableType::Metadata, "COV")) ==
          std::vector<std::string_view>{"coverArt"});
  }

  TEST_CASE("Completion - expands exact aliases to canonical variables")
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

  TEST_CASE("Completion - does not prefix match aliases")
  {
    CHECK(canonicalNames(completeQueryVariable(VariableType::Metadata, "t")) ==
          std::vector<std::string_view>{"title", "trackNumber", "trackTotal"});
    CHECK(canonicalNames(completeQueryVariable(VariableType::Metadata, "tn")) ==
          std::vector<std::string_view>{"trackNumber"});
    CHECK(canonicalNames(completeQueryVariable(VariableType::Metadata, "d")) ==
          std::vector<std::string_view>{"discNumber", "discTotal"});
  }

  TEST_CASE("FieldCatalog - resolves every descriptor through lookup and completion")
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
          auto const fieldRes = detail::resolveVariableField(descriptor.type, descriptor.canonicalName);
          REQUIRE(fieldRes.has_value());
          CHECK(*fieldRes == descriptor.field);

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
            auto const fieldRes = detail::resolveVariableField(descriptor.type, alias);
            REQUIRE(fieldRes.has_value());
            CHECK(*fieldRes == descriptor.field);

            auto const matches = completeQueryVariable(descriptor.type, alias);
            CHECK(containsMatch(matches, descriptor, QueryVariableCompletionMatchKind::ExactAlias));
          }
        }
      }
    }
  }

  TEST_CASE("Completion - returns nullopt for unknown variable fields")
  {
    // Known fields resolve identically to the diagnostic detail::resolveVariableField().
    CHECK(detail::lookupVariableField(VariableType::Metadata, "artist") == Field::ArtistId);
    CHECK(detail::lookupVariableField(VariableType::Property, "duration") == Field::Duration);

    // Tag and Custom variables always resolve to their fixed fields.
    CHECK(detail::lookupVariableField(VariableType::Tag, "anything") == Field::Tag);
    CHECK(detail::lookupVariableField(VariableType::Custom, "anything") == Field::Custom);

    // Unknown property/metadata names degrade to nullopt instead of throwing.
    CHECK_FALSE(detail::lookupVariableField(VariableType::Metadata, "not_a_field").has_value());
    CHECK_FALSE(detail::lookupVariableField(VariableType::Metadata, "musician"));
    CHECK(completeQueryVariable(VariableType::Metadata, "musician").empty());
    CHECK_FALSE(detail::lookupVariableField(VariableType::Property, "not_a_field").has_value());
  }

  TEST_CASE("FieldCatalog - exposes canonical names, fields and exact aliases in UI order")
  {
    struct Case final
    {
      std::string_view name;
      Field field;
      std::string_view alias{};
    };

    auto const check = [](VariableType type, std::span<Case const> expected)
    {
      auto const descriptors = queryVariableDescriptors(type);
      REQUIRE(descriptors.size() == expected.size());

      for (std::size_t i = 0; i < expected.size(); ++i)
      {
        auto const& [name, field, alias] = expected[i];
        CAPTURE(name);
        auto const& descriptor = descriptors[i];
        CHECK(descriptor.type == type);
        CHECK(descriptor.canonicalName == name);
        CHECK(descriptor.field == field);

        if (alias.empty())
        {
          CHECK(descriptor.aliases.empty());
        }
        else
        {
          REQUIRE(descriptor.aliases.size() == 1);
          CHECK(descriptor.aliases.front() == alias);
        }
      }
    };

    check(VariableType::Metadata,
          std::to_array<Case>({{"title", Field::Title, "t"},
                               {"artist", Field::ArtistId, "a"},
                               {"album", Field::AlbumId, "al"},
                               {"albumArtist", Field::AlbumArtistId, "aa"},
                               {"composer", Field::ComposerId, "c"},
                               {"conductor", Field::Conductor},
                               {"ensemble", Field::Ensemble},
                               {"work", Field::WorkId, "w"},
                               {"movement", Field::MovementId, "m"},
                               {"soloist", Field::Soloist},
                               {"recordingDate", Field::RecordingDate},
                               {"performer", Field::Performer},
                               {"credit", Field::Credit},
                               {"genre", Field::GenreId, "g"},
                               {"year", Field::Year, "y"},
                               {"trackNumber", Field::TrackNumber, "tn"},
                               {"trackTotal", Field::TrackTotal, "tt"},
                               {"discNumber", Field::DiscNumber, "dn"},
                               {"discTotal", Field::DiscTotal, "td"},
                               {"movementNumber", Field::MovementNumber, "mn"},
                               {"movementTotal", Field::MovementTotal, "mt"},
                               {"coverArt", Field::CoverArtId, "ca"}}));
    check(VariableType::Property,
          std::to_array<Case>({{"duration", Field::Duration, "l"},
                               {"bitrate", Field::Bitrate, "br"},
                               {"sampleRate", Field::SampleRate, "sr"},
                               {"channels", Field::Channels},
                               {"bitDepth", Field::BitDepth, "bd"},
                               {"codec", Field::Codec}}));
  }

  TEST_CASE("FieldCatalog - finds descriptors by canonical name, alias, and field")
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

  TEST_CASE("FieldCatalog - supplies canonical diagnostic names and formatted variable text")
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
