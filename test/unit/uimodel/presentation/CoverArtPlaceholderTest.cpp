// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/uimodel/presentation/CoverArtPlaceholder.h>

#include "test/unit/PresentationTextCatalogTestSupport.h"
#include <ao/rt/projection/TrackListProjection.h>
#include <ao/uimodel/library/presentation/TrackGroupHeadingPresentation.h>
#include <ao/uimodel/presentation/PresentationTextCatalog.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ao::uimodel::test
{
  TEST_CASE("CoverArtPlaceholder - style IDs and slot defaults are stable", "[uimodel][unit][cover-art]")
  {
    constexpr auto kExpected = std::array{
      CoverArtPlaceholderStyleEntry{CoverArtPlaceholderStyle::Monogram, "monogram"},
      CoverArtPlaceholderStyleEntry{CoverArtPlaceholderStyle::Note, "note"},
      CoverArtPlaceholderStyleEntry{CoverArtPlaceholderStyle::Vinyl, "vinyl"},
      CoverArtPlaceholderStyleEntry{CoverArtPlaceholderStyle::Equalizer, "equalizer"},
      CoverArtPlaceholderStyleEntry{CoverArtPlaceholderStyle::Soul, "soul"},
    };
    CHECK(kCoverArtPlaceholderStyles == kExpected);
    CHECK(coverArtPlaceholderStyleIds() == std::vector<std::string>{"monogram", "note", "vinyl", "equalizer", "soul"});

    for (auto const& [style, expectedId] : kExpected)
    {
      auto const id = coverArtPlaceholderStyleId(style);
      CHECK(id == expectedId);
      REQUIRE(parseCoverArtPlaceholderStyle(id));
      CHECK(*parseCoverArtPlaceholderStyle(id) == style);
    }

    CHECK_FALSE(parseCoverArtPlaceholderStyle("target"));
    CHECK(defaultCoverArtPlaceholderStyle(CoverArtPlaceholderSlot::GroupHeading) == CoverArtPlaceholderStyle::Monogram);
    CHECK(defaultCoverArtPlaceholderStyle(CoverArtPlaceholderSlot::Inspector) == CoverArtPlaceholderStyle::Vinyl);
    CHECK(defaultCoverArtPlaceholderStyle(CoverArtPlaceholderSlot::NowPlaying) == CoverArtPlaceholderStyle::Equalizer);
  }

  TEST_CASE("CoverArtPlaceholder - identity preserves candidate priority", "[uimodel][unit][cover-art]")
  {
    auto const candidates = std::array<std::string_view, 4>{"", "Synthetic Sun", "Parhelion", "2026"};
    auto const identity = makeCoverArtPlaceholderIdentity(candidates);

    CHECK(identity.primaryText == "Synthetic Sun");

    auto const presentation = makeCoverArtPlaceholderPresentation(CoverArtPlaceholderStyle::Monogram, identity);
    CHECK(presentation.monogram == "S");
    CHECK(presentation.monogramColor == CoverArtPlaceholderRgb{158, 103, 132});

    auto const samePresentation = makeCoverArtPlaceholderPresentation(CoverArtPlaceholderStyle::Monogram, identity);
    CHECK(samePresentation == presentation);

    auto const otherPresentation = makeCoverArtPlaceholderPresentation(
      CoverArtPlaceholderStyle::Monogram,
      makeCoverArtPlaceholderIdentity(std::array<std::string_view, 1>{"Parhelion"}));
    CHECK(otherPresentation.monogramColor == CoverArtPlaceholderRgb{134, 111, 171});
  }

  TEST_CASE("CoverArtPlaceholder - monogram handles ASCII Unicode and malformed input", "[uimodel][unit][cover-art]")
  {
    auto const ascii = makeCoverArtPlaceholderPresentation(
      CoverArtPlaceholderStyle::Monogram,
      makeCoverArtPlaceholderIdentity(std::array<std::string_view, 1>{"  synthetic"}));
    CHECK(ascii.monogram == "S");
    CHECK(ascii.monogramSize == CoverArtPlaceholderMonogramSize::Regular);

    auto const unicode = makeCoverArtPlaceholderPresentation(
      CoverArtPlaceholderStyle::Monogram, makeCoverArtPlaceholderIdentity(std::array<std::string_view, 1>{"海边"}));
    CHECK(unicode.monogram == "海");
    CHECK(unicode.monogramSize == CoverArtPlaceholderMonogramSize::Regular);

    auto const malformedText = std::string{"\xF0\x28\x8C\x28", 4};
    auto const malformed = makeCoverArtPlaceholderPresentation(
      CoverArtPlaceholderStyle::Monogram, CoverArtPlaceholderIdentity{.primaryText = malformedText});
    CHECK(malformed.monogram == "?");
    CHECK(malformed.monogramSize == CoverArtPlaceholderMonogramSize::Regular);

    auto const empty =
      makeCoverArtPlaceholderPresentation(CoverArtPlaceholderStyle::Monogram, CoverArtPlaceholderIdentity{});
    CHECK(empty.monogram == "?");
    CHECK(empty.monogramSize == CoverArtPlaceholderMonogramSize::Regular);
  }

  TEST_CASE("CoverArtPlaceholder - override monograms are normalized and bounded", "[uimodel][unit][cover-art]")
  {
    auto const compact = makeCoverArtPlaceholderPresentation(CoverArtPlaceholderStyle::Monogram,
                                                             CoverArtPlaceholderIdentity{
                                                               .primaryText = "identity",
                                                               .optMonogram = "  ab cd",
                                                             });
    CHECK(compact.monogram == "AB");
    CHECK(compact.monogramSize == CoverArtPlaceholderMonogramSize::Compact);

    auto const unicode = makeCoverArtPlaceholderPresentation(CoverArtPlaceholderStyle::Monogram,
                                                             CoverArtPlaceholderIdentity{
                                                               .primaryText = "identity",
                                                               .optMonogram = "海边风",
                                                             });
    CHECK(unicode.monogram == "海边");
    CHECK(unicode.monogramSize == CoverArtPlaceholderMonogramSize::Compact);

    auto const trailingWhitespace = makeCoverArtPlaceholderPresentation(CoverArtPlaceholderStyle::Monogram,
                                                                        CoverArtPlaceholderIdentity{
                                                                          .primaryText = "identity",
                                                                          .optMonogram = "a ",
                                                                        });
    CHECK(trailingWhitespace.monogram == "A");
    CHECK(trailingWhitespace.monogramSize == CoverArtPlaceholderMonogramSize::Regular);

    auto const empty = makeCoverArtPlaceholderPresentation(CoverArtPlaceholderStyle::Monogram,
                                                           CoverArtPlaceholderIdentity{
                                                             .primaryText = "identity",
                                                             .optMonogram = "",
                                                           });
    CHECK(empty.monogram == "?");
    CHECK(empty.monogramSize == CoverArtPlaceholderMonogramSize::Regular);

    auto const whitespace = makeCoverArtPlaceholderPresentation(CoverArtPlaceholderStyle::Monogram,
                                                                CoverArtPlaceholderIdentity{
                                                                  .primaryText = "identity",
                                                                  .optMonogram = " \t",
                                                                });
    CHECK(whitespace.monogram == "?");
    CHECK(whitespace.monogramSize == CoverArtPlaceholderMonogramSize::Regular);

    auto const malformedText = std::string{"\xF0\x28\x8C\x28", 4};
    auto const malformed = makeCoverArtPlaceholderPresentation(CoverArtPlaceholderStyle::Monogram,
                                                               CoverArtPlaceholderIdentity{
                                                                 .primaryText = "identity",
                                                                 .optMonogram = malformedText,
                                                               });
    CHECK(malformed.monogram == "?");
    CHECK(malformed.monogramSize == CoverArtPlaceholderMonogramSize::Regular);

    auto malformedTail = std::string{"ab"};
    malformedTail.append(malformedText);
    auto const malformedAfterLimit = makeCoverArtPlaceholderPresentation(CoverArtPlaceholderStyle::Monogram,
                                                                         CoverArtPlaceholderIdentity{
                                                                           .primaryText = "identity",
                                                                           .optMonogram = malformedTail,
                                                                         });
    CHECK(malformedAfterLimit.monogram == "?");
    CHECK(malformedAfterLimit.monogramSize == CoverArtPlaceholderMonogramSize::Regular);
  }

  TEST_CASE("CoverArtPlaceholder - group headings preserve semantic monograms", "[uimodel][unit][cover-art]")
  {
    auto const& textCatalog = ao::test::englishPresentationTextCatalog();

    auto const yearHeading = rt::TrackGroupHeading{.primary = std::uint16_t{2023}};
    auto const yearText = formatTrackGroupHeading(textCatalog, yearHeading);
    auto const optYearMonogram = trackGroupCoverArtMonogram(yearHeading);
    REQUIRE(optYearMonogram);
    auto const yearIdentity = CoverArtPlaceholderIdentity{
      .primaryText = yearText.primaryText,
      .optMonogram = optYearMonogram,
    };
    CHECK(yearIdentity.primaryText == "2023");
    CHECK(*optYearMonogram == "23");

    auto const yearPresentation = makeCoverArtPlaceholderPresentation(CoverArtPlaceholderStyle::Monogram, yearIdentity);
    CHECK(yearPresentation.monogram == "23");
    CHECK(yearPresentation.monogramSize == CoverArtPlaceholderMonogramSize::Compact);
    CHECK(yearPresentation.monogramColor == CoverArtPlaceholderRgb{144, 135, 70});

    auto const optTwoDigitYearMonogram =
      trackGroupCoverArtMonogram(rt::TrackGroupHeading{.primary = std::uint16_t{99}});
    REQUIRE(optTwoDigitYearMonogram);
    CHECK(*optTwoDigitYearMonogram == "99");

    auto const optOneDigitYearMonogram = trackGroupCoverArtMonogram(rt::TrackGroupHeading{.primary = std::uint16_t{5}});
    REQUIRE(optOneDigitYearMonogram);
    CHECK(*optOneDigitYearMonogram == "5");
    CHECK(makeCoverArtPlaceholderPresentation(CoverArtPlaceholderStyle::Monogram,
                                              CoverArtPlaceholderIdentity{
                                                .primaryText = "5",
                                                .optMonogram = optOneDigitYearMonogram,
                                              })
            .monogramSize == CoverArtPlaceholderMonogramSize::Regular);

    auto const missingHeading = rt::TrackGroupHeading{.primary = rt::MissingTrackValueKind::Year};
    auto const missingText = formatTrackGroupHeading(textCatalog, missingHeading);
    auto const optMissingMonogram = trackGroupCoverArtMonogram(missingHeading);
    REQUIRE(optMissingMonogram);
    auto const missingIdentity = CoverArtPlaceholderIdentity{
      .primaryText = missingText.primaryText,
      .optMonogram = optMissingMonogram,
    };
    CHECK(missingIdentity.primaryText == "Unknown Year");
    CHECK(*optMissingMonogram == "?");
    auto const missingPresentation =
      makeCoverArtPlaceholderPresentation(CoverArtPlaceholderStyle::Monogram, missingIdentity);
    CHECK(missingPresentation.monogram == "?");
    CHECK(missingPresentation.monogramSize == CoverArtPlaceholderMonogramSize::Regular);
  }
} // namespace ao::uimodel::test
