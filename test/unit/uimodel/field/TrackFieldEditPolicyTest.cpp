// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/StateTypes.h>
#include <ao/rt/TrackField.h>
#include <ao/uimodel/field/TrackFieldEditCodec.h>
#include <ao/uimodel/field/TrackFieldEditPolicy.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace ao::uimodel::test
{
  namespace
  {
    using StringTarget = std::optional<std::string> rt::MetadataPatch::*;
    using Uint16Target = std::optional<std::uint16_t> rt::MetadataPatch::*;

    void checkStringField(rt::TrackField field, StringTarget target)
    {
      INFO("Field: " << rt::trackFieldId(field));
      auto patch = rt::MetadataPatch{};
      auto const value = TrackFieldEditValue{std::in_place_type<std::string>, "Edited"};

      CHECK(trackFieldCanWritePatch(field));
      CHECK(writeTrackFieldPatch(patch, field, value));
      REQUIRE((patch.*target).has_value());
      CHECK(*(patch.*target) == "Edited");
    }

    void checkUint16Field(rt::TrackField field, Uint16Target target)
    {
      INFO("Field: " << rt::trackFieldId(field));
      auto patch = rt::MetadataPatch{};
      auto const value = TrackFieldEditValue{std::in_place_type<std::uint16_t>, static_cast<std::uint16_t>(42)};

      CHECK(trackFieldCanWritePatch(field));
      CHECK(writeTrackFieldPatch(patch, field, value));
      REQUIRE((patch.*target).has_value());
      CHECK(*(patch.*target) == 42);
    }
  } // namespace

  TEST_CASE("writeTrackFieldPatch writes editable metadata fields", "[uimodel][unit][field][edit]")
  {
    checkStringField(rt::TrackField::Title, &rt::MetadataPatch::optTitle);
    checkStringField(rt::TrackField::Artist, &rt::MetadataPatch::optArtist);
    checkStringField(rt::TrackField::Album, &rt::MetadataPatch::optAlbum);
    checkStringField(rt::TrackField::AlbumArtist, &rt::MetadataPatch::optAlbumArtist);
    checkStringField(rt::TrackField::Genre, &rt::MetadataPatch::optGenre);
    checkStringField(rt::TrackField::Composer, &rt::MetadataPatch::optComposer);
    checkStringField(rt::TrackField::Work, &rt::MetadataPatch::optWork);
    checkStringField(rt::TrackField::Movement, &rt::MetadataPatch::optMovement);

    checkUint16Field(rt::TrackField::Year, &rt::MetadataPatch::optYear);
    checkUint16Field(rt::TrackField::DiscNumber, &rt::MetadataPatch::optDiscNumber);
    checkUint16Field(rt::TrackField::DiscTotal, &rt::MetadataPatch::optDiscTotal);
    checkUint16Field(rt::TrackField::TrackNumber, &rt::MetadataPatch::optTrackNumber);
    checkUint16Field(rt::TrackField::TrackTotal, &rt::MetadataPatch::optTrackTotal);
    checkUint16Field(rt::TrackField::MovementNumber, &rt::MetadataPatch::optMovementNumber);
    checkUint16Field(rt::TrackField::MovementTotal, &rt::MetadataPatch::optMovementTotal);
  }

  TEST_CASE("writeTrackFieldPatch rejects wrong edit value variants without mutation", "[uimodel][unit][field][edit]")
  {
    auto textPatch = rt::MetadataPatch{};
    textPatch.optTitle = "Before";
    auto const numericValue = TrackFieldEditValue{std::in_place_type<std::uint16_t>, static_cast<std::uint16_t>(7)};

    CHECK_FALSE(writeTrackFieldPatch(textPatch, rt::TrackField::Title, numericValue));
    REQUIRE(textPatch.optTitle.has_value());
    CHECK(*textPatch.optTitle == "Before");

    auto numberPatch = rt::MetadataPatch{};
    numberPatch.optYear = static_cast<std::uint16_t>(1999);
    auto const stringValue = TrackFieldEditValue{std::in_place_type<std::string>, "Not a number"};

    CHECK_FALSE(writeTrackFieldPatch(numberPatch, rt::TrackField::Year, stringValue));
    REQUIRE(numberPatch.optYear.has_value());
    CHECK(*numberPatch.optYear == 1999);
  }

  TEST_CASE("writeTrackFieldPatch rejects read-only and synthetic fields without mutation",
            "[uimodel][unit][field][edit]")
  {
    auto patch = rt::MetadataPatch{};
    patch.optTitle = "Before";
    auto const value = TrackFieldEditValue{std::in_place_type<std::string>, "Edited"};

    CHECK_FALSE(trackFieldCanWritePatch(rt::TrackField::Tags));
    CHECK_FALSE(trackFieldCanWritePatch(rt::TrackField::Duration));
    CHECK_FALSE(trackFieldCanWritePatch(rt::TrackField::Quality));

    CHECK_FALSE(writeTrackFieldPatch(patch, rt::TrackField::Tags, value));
    CHECK_FALSE(writeTrackFieldPatch(patch, rt::TrackField::Duration, value));
    CHECK_FALSE(writeTrackFieldPatch(patch, rt::TrackField::Quality, value));

    REQUIRE(patch.optTitle.has_value());
    CHECK(*patch.optTitle == "Before");
  }
} // namespace ao::uimodel::test
