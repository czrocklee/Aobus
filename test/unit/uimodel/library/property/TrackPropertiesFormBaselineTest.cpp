// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibrarySnapshot.h>
#include <ao/uimodel/library/property/TrackPropertiesFormModel.h>
#include <ao/uimodel/library/property/TrackPropertiesFormSpec.h>
#include <ao/uimodel/library/track/TrackAuthoring.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <utility>

namespace ao::uimodel::test
{
  namespace
  {
    struct BaselineFixture final
    {
      rt::test::MusicLibraryFixture storage{};
      rt::LibraryChanges changes{rt::test::makeStateOnlyLibraryChanges(storage.library())};
      rt::test::LibraryCommandsFixture commands{storage.library(), changes};

      TrackId addTrack(library::test::TrackSpec const& spec) { return commands.addTrack(spec); }
    };

    TrackFieldEditValue textEdit(std::string value)
    {
      return TrackFieldEditValue{std::in_place_type<std::string>, std::move(value)};
    }

    TrackFieldEditValue numberEdit(std::uint16_t const value)
    {
      return TrackFieldEditValue{std::in_place_type<std::uint16_t>, value};
    }
  } // namespace

  TEST_CASE("loadTrackPropertiesFormBaseline aggregates common and mixed standard fields", "[uimodel][unit][property]")
  {
    auto fixture = BaselineFixture{};
    auto const firstId = fixture.addTrack({.title = "Same", .album = "First", .uri = "first.flac"});
    auto const secondId = fixture.addTrack({.title = "Same", .album = "Second", .uri = "second.flac"});
    auto const ids = std::array{firstId, secondId};
    auto const spec = buildTrackPropertiesFormSpec(ao::test::englishMessageCatalog());
    auto form = TrackPropertiesFormModel{ao::test::englishMessageCatalog()};
    auto snapshot = fixture.commands.library().snapshot();

    REQUIRE(loadTrackPropertiesFormBaseline(snapshot, ids, spec, form));

    auto const title = form.rowView(rt::TrackField::Title);
    CHECK_FALSE(title.mixed);
    CHECK(title.text == "Same");
    CHECK(title.editable);

    auto const album = form.rowView(rt::TrackField::Album);
    CHECK(album.mixed);
    CHECK(album.editable);
  }

  TEST_CASE("loadTrackPropertiesFormBaseline preserves empty text and numeric zero values", "[uimodel][unit][property]")
  {
    auto fixture = BaselineFixture{};
    auto const firstId = fixture.addTrack({.title = "", .uri = "first.flac", .year = 0});
    auto const secondId = fixture.addTrack({.title = "", .uri = "second.flac", .year = 0});
    auto const ids = std::array{firstId, secondId};
    auto const spec = buildTrackPropertiesFormSpec(ao::test::englishMessageCatalog());
    auto form = TrackPropertiesFormModel{ao::test::englishMessageCatalog()};
    auto snapshot = fixture.commands.library().snapshot();

    REQUIRE(loadTrackPropertiesFormBaseline(snapshot, ids, spec, form));

    auto const title = form.rowView(rt::TrackField::Title);
    CHECK_FALSE(title.mixed);
    CHECK(title.text.empty());

    auto const year = form.rowView(rt::TrackField::Year);
    CHECK_FALSE(year.mixed);
    CHECK(year.text.empty());

    form.setEditValue(rt::TrackField::Title, textEdit(""));
    form.setEditValue(rt::TrackField::Year, numberEdit(0));
    CHECK_FALSE(form.canSave());
  }

  TEST_CASE("loadTrackPropertiesFormBaseline keeps read-only rows out of patches", "[uimodel][unit][property]")
  {
    auto fixture = BaselineFixture{};
    auto const trackId = fixture.addTrack({.title = "Track", .uri = "readonly.flac"});
    auto const spec = buildTrackPropertiesFormSpec(ao::test::englishMessageCatalog());
    auto form = TrackPropertiesFormModel{ao::test::englishMessageCatalog()};
    auto snapshot = fixture.commands.library().snapshot();

    REQUIRE(loadTrackPropertiesFormBaseline(snapshot, std::array{trackId}, spec, form));
    form.setExplicitFieldEdit(rt::TrackField::FilePath, textEdit("replacement.flac"));

    CHECK_FALSE(form.rowView(rt::TrackField::FilePath).editable);
    CHECK_FALSE(form.canSave());
    auto const patch = form.buildPatch();
    CHECK_FALSE(patch.optTitle);
    CHECK(patch.customUpdates.empty());
  }

  TEST_CASE("loadTrackPropertiesFormBaseline preserves mixed fields until an explicit override",
            "[uimodel][unit][property]")
  {
    auto fixture = BaselineFixture{};
    auto const firstId = fixture.addTrack({.title = "First", .uri = "first.flac"});
    auto const secondId = fixture.addTrack({.title = "Second", .uri = "second.flac"});
    auto const ids = std::array{firstId, secondId};
    auto const spec = buildTrackPropertiesFormSpec(ao::test::englishMessageCatalog());
    auto form = TrackPropertiesFormModel{ao::test::englishMessageCatalog()};
    auto snapshot = fixture.commands.library().snapshot();

    REQUIRE(loadTrackPropertiesFormBaseline(snapshot, ids, spec, form));
    form.setEditValue(rt::TrackField::Title, textEdit("Replacement"));
    CHECK_FALSE(form.buildPatch().optTitle);

    form.setExplicitFieldEdit(rt::TrackField::Title, textEdit("Replacement"));
    auto const patch = form.buildPatch();
    REQUIRE(patch.optTitle);
    CHECK(*patch.optTitle == "Replacement");
  }

  TEST_CASE("loadTrackPropertiesFormBaseline rejects incomplete targets without changing the form",
            "[uimodel][unit][property]")
  {
    auto fixture = BaselineFixture{};
    auto const trackId = fixture.addTrack({.title = "Original", .uri = "present.flac"});
    auto const spec = buildTrackPropertiesFormSpec(ao::test::englishMessageCatalog());
    auto form = TrackPropertiesFormModel{ao::test::englishMessageCatalog()};
    auto snapshot = fixture.commands.library().snapshot();
    REQUIRE(loadTrackPropertiesFormBaseline(snapshot, std::array{trackId}, spec, form));
    form.setEditValue(rt::TrackField::Title, textEdit("Pending"));

    SECTION("the first target is missing")
    {
      auto const ids = std::array{TrackId{999998}, trackId};
      auto const res = loadTrackPropertiesFormBaseline(snapshot, ids, spec, form);
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::NotFound);
    }

    SECTION("a later target is missing")
    {
      auto const ids = std::array{trackId, TrackId{999999}};
      auto const res = loadTrackPropertiesFormBaseline(snapshot, ids, spec, form);
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::NotFound);
    }

    SECTION("the target sequence is empty")
    {
      auto const res = loadTrackPropertiesFormBaseline(snapshot, std::span<TrackId const>{}, spec, form);
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::InvalidInput);
    }

    REQUIRE(form.buildPatch().optTitle);
    CHECK(*form.buildPatch().optTitle == "Pending");
    CHECK(form.rowView(rt::TrackField::Title).text == "Original");
  }

  TEST_CASE("loadTrackPropertiesFormBaseline replaces prepopulated edits on a repeated successful load",
            "[uimodel][unit][property]")
  {
    auto fixture = BaselineFixture{};
    auto const firstId = fixture.addTrack({.title = "First", .uri = "first.flac"});
    auto const secondId = fixture.addTrack({.title = "Second", .uri = "second.flac"});
    auto const spec = buildTrackPropertiesFormSpec(ao::test::englishMessageCatalog());
    auto form = TrackPropertiesFormModel{ao::test::englishMessageCatalog()};
    auto snapshot = fixture.commands.library().snapshot();

    REQUIRE(loadTrackPropertiesFormBaseline(snapshot, std::array{firstId}, spec, form));
    form.setEditValue(rt::TrackField::Title, textEdit("Pending"));
    REQUIRE(form.canSave());

    REQUIRE(loadTrackPropertiesFormBaseline(snapshot, std::array{secondId}, spec, form));
    CHECK(form.rowView(rt::TrackField::Title).text == "Second");
    CHECK_FALSE(form.rowView(rt::TrackField::Title).mixed);
    CHECK_FALSE(form.canSave());
  }

  TEST_CASE("loadTrackPropertiesFormBaseline aggregates duplicate targets without changing common or mixed values",
            "[uimodel][unit][property]")
  {
    auto fixture = BaselineFixture{};
    auto const firstId = fixture.addTrack({.title = "First", .uri = "first.flac"});
    auto const secondId = fixture.addTrack({.title = "Second", .uri = "second.flac"});
    auto const spec = buildTrackPropertiesFormSpec(ao::test::englishMessageCatalog());
    auto form = TrackPropertiesFormModel{ao::test::englishMessageCatalog()};
    auto snapshot = fixture.commands.library().snapshot();
    auto const duplicateIds = std::array{secondId, secondId, secondId};

    REQUIRE(loadTrackPropertiesFormBaseline(snapshot, duplicateIds, spec, form));
    CHECK(form.rowView(rt::TrackField::Title).text == "Second");
    CHECK_FALSE(form.rowView(rt::TrackField::Title).mixed);
    form.setEditValue(rt::TrackField::Title, textEdit("Second"));
    CHECK_FALSE(form.canSave());

    auto const orderedIds = std::array{secondId, firstId, secondId};
    REQUIRE(loadTrackPropertiesFormBaseline(snapshot, orderedIds, spec, form));
    CHECK(form.rowView(rt::TrackField::Title).mixed);
  }
} // namespace ao::uimodel::test
