// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/presentation/TrackColumnDefaults.h>

#include "test/unit/MessageCatalogTestSupport.h"
#include <ao/rt/TrackField.h>
#include <ao/uimodel/library/presentation/TrackPresentationText.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>

namespace ao::uimodel::test
{
  namespace
  {
    using F = rt::TrackField;

    // Free text grows with the view; bounded scalars read right-aligned; short
    // technical text keeps a fixed column but reads from the start.
    constexpr auto kFlexibleTextFields = std::to_array<F>({
      F::Title,
      F::Artist,
      F::Album,
      F::AlbumArtist,
      F::Genre,
      F::Composer,
      F::Conductor,
      F::Ensemble,
      F::Work,
      F::Movement,
      F::Soloist,
      F::Tags,
      F::FilePath,
    });

    constexpr auto kScalarFields = std::to_array<F>({
      F::Year,
      F::DiscNumber,
      F::DiscTotal,
      F::TrackNumber,
      F::TrackTotal,
      F::MovementNumber,
      F::MovementTotal,
      F::Duration,
      F::SampleRate,
      F::Channels,
      F::BitDepth,
      F::Bitrate,
      F::FileSize,
      F::ModifiedTime,
      F::DisplayTrackNumber,
    });

    constexpr auto kFixedTextFields = std::to_array<F>({F::Codec, F::TechnicalSummary, F::Quality});
  } // namespace

  TEST_CASE("trackColumnDefaults sizes and aligns every presentable field by its value role",
            "[uimodel][unit][presentation]")
  {
    for (auto const& definition : rt::trackFieldDefinitions())
    {
      if (!definition.presentable)
      {
        continue;
      }

      INFO("Track Field " << definition.id);
      auto const defaults = trackColumnDefaults(definition.field);
      auto const isFlexibleText = std::ranges::contains(kFlexibleTextFields, definition.field);
      auto const isScalar = std::ranges::contains(kScalarFields, definition.field);
      auto const isFixedText = std::ranges::contains(kFixedTextFields, definition.field);
      REQUIRE(std::ranges::count(std::array{isFlexibleText, isScalar, isFixedText}, true) == 1);

      CHECK(defaults.sizing == (isFlexibleText ? TrackColumnSizing::Flexible : TrackColumnSizing::Fixed));
      CHECK(defaults.alignment == (isScalar ? TrackColumnAlignment::End : TrackColumnAlignment::Start));
      CHECK(defaults.minimumWidth > 0);
      CHECK(defaults.minimumWidth < defaults.width);
      CHECK(defaults.weight > 0.0);

      if (!isFlexibleText)
      {
        CHECK(defaults.minimumWidth == TrackColumnDefaults::kDefaultMinimumWidth);
        CHECK(defaults.weight == 1.0);
      }
    }
  }

  TEST_CASE("trackColumnDefaults gives flexible text one shared minimum above fixed columns",
            "[uimodel][unit][presentation]")
  {
    auto const sharedMinimum = trackColumnDefaults(F::Title).minimumWidth;
    CHECK(sharedMinimum > TrackColumnDefaults::kDefaultMinimumWidth);

    for (auto const field : kFlexibleTextFields)
    {
      INFO("Track Field " << rt::trackFieldId(field));
      CHECK(trackColumnDefaults(field).minimumWidth == sharedMinimum);
    }
  }

  TEST_CASE("trackColumnDefaults weights primary text above secondary text", "[uimodel][unit][presentation]")
  {
    auto const weight = [](F const field) { return trackColumnDefaults(field).weight; };

    CHECK(weight(F::Title) > weight(F::Artist));
    CHECK(weight(F::Artist) == weight(F::Album));
    CHECK(weight(F::Album) > weight(F::AlbumArtist));
    CHECK(weight(F::AlbumArtist) > weight(F::Tags));
    CHECK(weight(F::Tags) > weight(F::Genre));

    for (auto const field : {F::Composer, F::Conductor, F::Ensemble, F::Work, F::Movement, F::Soloist})
    {
      INFO("Track Field " << rt::trackFieldId(field));
      CHECK(weight(field) == weight(F::Genre));
    }
  }

  TEST_CASE("trackFieldColumnTitle uses the localized field label", "[uimodel][unit][presentation]")
  {
    auto const& textCatalog = ao::test::englishMessageCatalog();
    CHECK(trackFieldColumnTitle(textCatalog, F::AlbumArtist) == "Album Artist");

    for (auto const& definition : rt::trackFieldDefinitions())
    {
      if (!definition.presentable)
      {
        continue;
      }

      INFO("Track Field " << definition.id);
      CHECK_FALSE(trackFieldColumnTitle(textCatalog, definition.field).empty());
      CHECK(trackFieldColumnTitle(textCatalog, definition.field) == trackFieldLabel(textCatalog, definition.field));
    }
  }
} // namespace ao::uimodel::test
