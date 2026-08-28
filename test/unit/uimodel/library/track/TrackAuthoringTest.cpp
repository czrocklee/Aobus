// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/track/TrackAuthoring.h>

#include <ao/rt/TrackField.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/projection/TrackDetailSnapshot.h>
#include <ao/uimodel/field/TrackFieldFormatter.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>

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

      CHECK(canWriteTrackFieldPatch(field));
      CHECK(writeTrackFieldPatch(patch, field, value));
      REQUIRE((patch.*target).has_value());
      CHECK(*(patch.*target) == "Edited");
    }

    void checkUint16Field(rt::TrackField field, Uint16Target target)
    {
      INFO("Field: " << rt::trackFieldId(field));
      auto patch = rt::MetadataPatch{};
      auto const value = TrackFieldEditValue{std::in_place_type<std::uint16_t>, static_cast<std::uint16_t>(42)};

      CHECK(canWriteTrackFieldPatch(field));
      CHECK(writeTrackFieldPatch(patch, field, value));
      REQUIRE((patch.*target).has_value());
      CHECK(*(patch.*target) == 42);
    }
  } // namespace

  TEST_CASE("TrackAuthoring - preserves text edit input", "[uimodel][unit][track-authoring][codec]")
  {
    auto const editValue = makeTextEditValue(" Test ");
    auto const* text = std::get_if<std::string>(&editValue);
    REQUIRE(text != nullptr);
    CHECK(*text == " Test ");

    auto const parsedRes = parseTextEditValue("New Title");
    REQUIRE(parsedRes.has_value());
    auto const* parsedText = std::get_if<std::string>(&*parsedRes);
    REQUIRE(parsedText != nullptr);
    CHECK(*parsedText == "New Title");
  }

  TEST_CASE("TrackAuthoring - parses uint16 edit text", "[uimodel][unit][track-authoring][codec]")
  {
    SECTION("valid number")
    {
      auto const result = parseUint16EditValue("  42  ");
      REQUIRE(result.has_value());
      auto const* value = std::get_if<std::uint16_t>(&*result);
      REQUIRE(value != nullptr);
      CHECK(*value == 42);
    }

    SECTION("empty input clears to zero")
    {
      auto const result = parseUint16EditValue("    ");
      REQUIRE(result.has_value());
      auto const* value = std::get_if<std::uint16_t>(&*result);
      REQUIRE(value != nullptr);
      CHECK(*value == 0);
    }

    SECTION("invalid number returns a rejected format error")
    {
      auto const result = parseUint16EditValue("abc");

      REQUIRE_FALSE(result.has_value());
      CHECK(result.error().code == Error::Code::FormatRejected);
      CHECK(result.error().message == "Enter a whole number from 0 to 65535.");
    }

    SECTION("negative input is rejected")
    {
      CHECK_FALSE(parseUint16EditValue("-1").has_value());
    }

    SECTION("out-of-range input is rejected")
    {
      CHECK_FALSE(parseUint16EditValue("65536").has_value());
    }
  }

  TEST_CASE("TrackAuthoring - writes editable metadata fields", "[uimodel][unit][track-authoring][patch]")
  {
    checkStringField(rt::TrackField::Title, &rt::MetadataPatch::optTitle);
    checkStringField(rt::TrackField::Artist, &rt::MetadataPatch::optArtist);
    checkStringField(rt::TrackField::Album, &rt::MetadataPatch::optAlbum);
    checkStringField(rt::TrackField::AlbumArtist, &rt::MetadataPatch::optAlbumArtist);
    checkStringField(rt::TrackField::Genre, &rt::MetadataPatch::optGenre);
    checkStringField(rt::TrackField::Composer, &rt::MetadataPatch::optComposer);
    checkStringField(rt::TrackField::Conductor, &rt::MetadataPatch::optConductor);
    checkStringField(rt::TrackField::Ensemble, &rt::MetadataPatch::optEnsemble);
    checkStringField(rt::TrackField::Work, &rt::MetadataPatch::optWork);
    checkStringField(rt::TrackField::Movement, &rt::MetadataPatch::optMovement);
    checkStringField(rt::TrackField::Soloist, &rt::MetadataPatch::optSoloist);

    checkUint16Field(rt::TrackField::Year, &rt::MetadataPatch::optYear);
    checkUint16Field(rt::TrackField::DiscNumber, &rt::MetadataPatch::optDiscNumber);
    checkUint16Field(rt::TrackField::DiscTotal, &rt::MetadataPatch::optDiscTotal);
    checkUint16Field(rt::TrackField::TrackNumber, &rt::MetadataPatch::optTrackNumber);
    checkUint16Field(rt::TrackField::TrackTotal, &rt::MetadataPatch::optTrackTotal);
    checkUint16Field(rt::TrackField::MovementNumber, &rt::MetadataPatch::optMovementNumber);
    checkUint16Field(rt::TrackField::MovementTotal, &rt::MetadataPatch::optMovementTotal);
  }

  TEST_CASE("TrackAuthoring - rejects wrong edit value variants without mutation",
            "[uimodel][unit][track-authoring][patch]")
  {
    auto textPatch = rt::MetadataPatch{};
    textPatch.optTitle = "Before";
    auto const numericValue = TrackFieldEditValue{std::in_place_type<std::uint16_t>, static_cast<std::uint16_t>(7)};

    CHECK_FALSE(writeTrackFieldPatch(textPatch, rt::TrackField::Title, numericValue));
    REQUIRE(textPatch.optTitle);
    CHECK(*textPatch.optTitle == "Before");

    auto numberPatch = rt::MetadataPatch{};
    numberPatch.optYear = static_cast<std::uint16_t>(1999);
    auto const stringValue = TrackFieldEditValue{std::in_place_type<std::string>, "Not a number"};

    CHECK_FALSE(writeTrackFieldPatch(numberPatch, rt::TrackField::Year, stringValue));
    REQUIRE(numberPatch.optYear);
    CHECK(*numberPatch.optYear == 1999);
  }

  TEST_CASE("TrackAuthoring - rejects read-only and synthetic fields without mutation",
            "[uimodel][unit][track-authoring][patch]")
  {
    auto patch = rt::MetadataPatch{};
    patch.optTitle = "Before";
    auto const value = TrackFieldEditValue{std::in_place_type<std::string>, "Edited"};

    CHECK_FALSE(canWriteTrackFieldPatch(rt::TrackField::Tags));
    CHECK_FALSE(canWriteTrackFieldPatch(rt::TrackField::Duration));
    CHECK_FALSE(canWriteTrackFieldPatch(rt::TrackField::Quality));

    CHECK_FALSE(writeTrackFieldPatch(patch, rt::TrackField::Tags, value));
    CHECK_FALSE(writeTrackFieldPatch(patch, rt::TrackField::Duration, value));
    CHECK_FALSE(writeTrackFieldPatch(patch, rt::TrackField::Quality, value));

    REQUIRE(patch.optTitle);
    CHECK(*patch.optTitle == "Before");
  }

  TEST_CASE("TrackAuthoring - protects aggregate sentinel values", "[uimodel][unit][track-authoring][inline-edit]")
  {
    constexpr auto kLocalizedMixedText = "Mehrere Werte";
    auto snap = rt::TrackDetailSnapshot{};

    CHECK(isProtectedInlineEditText(rt::TrackField::Title, snap, kLocalizedMixedText, kLocalizedMixedText, false));
    CHECK_FALSE(
      isProtectedInlineEditText(rt::TrackField::Title, snap, "<Multiple Values>", kLocalizedMixedText, false));

    rt::trackFieldArrayAt(snap.fields, rt::TrackField::Title).mixed = true;
    CHECK(
      isProtectedInlineEditText(rt::TrackField::Title, snap, kCompositeMixedTrackText, kCompositeMixedTrackText, true));
    CHECK(isProtectedInlineEditText(
      rt::TrackField::Title, snap, kCompositeMixedTrackText, kCompositeMixedTrackText, false));
    CHECK_FALSE(
      isProtectedInlineEditText(rt::TrackField::Title, snap, "anything else", kCompositeMixedTrackText, true));

    rt::trackFieldArrayAt(snap.fields, rt::TrackField::Title).mixed = false;
    CHECK_FALSE(
      isProtectedInlineEditText(rt::TrackField::Title, snap, kCompositeMixedTrackText, kCompositeMixedTrackText, true));
    CHECK_FALSE(isProtectedInlineEditText(rt::TrackField::Title, snap, "edit", kCompositeMixedTrackText, true));
    CHECK_FALSE(isProtectedInlineEditText(rt::TrackField::Title, snap, "", kCompositeMixedTrackText, true));
  }
} // namespace ao::uimodel::test
