// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/uimodel/library/track/TrackAuthoringTestSupport.h"
#include <ao/Error.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/rt/library/LibrarySnapshot.h>
#include <ao/uimodel/library/property/TrackPropertiesFormModel.h>
#include <ao/uimodel/library/property/TrackPropertiesFormSpec.h>
#include <ao/uimodel/library/track/TrackAuthoringSessions.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace ao::uimodel::test
{
  TEST_CASE("TrackAuthoringSession snapshot - prepares an owning mixed baseline at one bound revision",
            "[uimodel][unit][library-authoring]")
  {
    auto fixture = TrackAuthoringFixture{2};
    auto const targetIds = std::array{fixture.trackIds()[1], fixture.trackIds()[0], fixture.trackIds()[1]};
    auto form = TrackPropertiesFormModel{ao::test::englishMessageCatalog()};
    auto const spec = buildTrackPropertiesFormSpec(ao::test::englishMessageCatalog());
    std::uint64_t baselineRevision = 0;
    auto session = [&]
    {
      auto snapshot = fixture.library().snapshot();
      baselineRevision = snapshot.revision();
      auto sessionRes = TrackAuthoringSession::begin(fixture.library(), targetIds, snapshot);
      REQUIRE(sessionRes);
      CHECK(sessionRes->boundRevision() == snapshot.revision());
      REQUIRE(loadTrackPropertiesFormBaseline(snapshot, sessionRes->targetIds(), spec, form));
      return std::move(*sessionRes);
    }();

    // The form and session survive the caller's released snapshot without borrowing it.
    CHECK(session.boundRevision() == baselineRevision);
    CHECK(std::ranges::equal(session.targetIds(), targetIds));
    CHECK(session.isCurrent());
    CHECK(form.rowView(rt::TrackField::Title).mixed);
    CHECK(form.rowView(rt::TrackField::Title).editable);
    CHECK_FALSE(form.canSave());
    form.setExplicitFieldEdit(rt::TrackField::Title, std::string{"Draft"});
    CHECK(form.buildPatch().optTitle == "Draft");
    CHECK(form.rowView(rt::TrackField::Title).mixed);
  }

  TEST_CASE("TrackAuthoringSession snapshot - released baseline permits no-op and applied saves but not stale drafts",
            "[uimodel][integration][library-authoring]")
  {
    auto fixture = TrackAuthoringFixture{1};
    auto form = TrackPropertiesFormModel{ao::test::englishMessageCatalog()};
    auto const spec = buildTrackPropertiesFormSpec(ao::test::englishMessageCatalog());
    auto session = [&]
    {
      auto snapshot = fixture.library().snapshot();
      auto sessionRes = TrackAuthoringSession::begin(fixture.library(), fixture.trackIds(), snapshot);
      REQUIRE(sessionRes);
      CHECK(sessionRes->boundRevision() == snapshot.revision());
      REQUIRE(loadTrackPropertiesFormBaseline(snapshot, sessionRes->targetIds(), spec, form));
      return std::move(*sessionRes);
    }();
    auto const revision = session.boundRevision();
    CHECK(form.rowView(rt::TrackField::Title).text == "Old Title");
    std::size_t invalidatedCount = 0;
    auto subscription = session.onInvalidated([&invalidatedCount] noexcept { ++invalidatedCount; });

    auto submitRes = fixture.runTask(session.submitMetadataAsync(rt::MetadataPatch{.optTitle = "Old Title"}));
    REQUIRE(submitRes);
    CHECK(submitRes->status == rt::AuthoringStatus::NoOp);
    CHECK(session.isCurrent());
    CHECK(session.boundRevision() == revision);
    CHECK(invalidatedCount == 0);
    CHECK(fixture.title(fixture.trackIds().front()) == "Old Title");

    form.setEditValue(rt::TrackField::Title, std::string{"Committed"});
    submitRes = fixture.runTask(session.submitMetadataAsync(form.buildPatch()));
    REQUIRE(submitRes);
    CHECK(submitRes->status == rt::AuthoringStatus::Applied);
    CHECK(session.isCurrent());
    CHECK(session.boundRevision() == revision + 1);
    CHECK(invalidatedCount == 0);
    CHECK(fixture.title(fixture.trackIds().front()) == "Committed");
    CHECK(form.rowView(rt::TrackField::Title).text == "Old Title");

    auto otherRes = TrackAuthoringSession::begin(fixture.library(), fixture.trackIds());
    REQUIRE(otherRes);
    auto const otherSubmitRes = fixture.runTask(otherRes->submitMetadataAsync(rt::MetadataPatch{.optTitle = "Newer"}));
    REQUIRE(otherSubmitRes);
    CHECK(otherSubmitRes->status == rt::AuthoringStatus::Applied);
    CHECK_FALSE(session.isCurrent());
    CHECK(invalidatedCount == 1);
    form.setEditValue(rt::TrackField::Title, std::string{"Stale draft"});

    submitRes = fixture.runTask(session.submitMetadataAsync(form.buildPatch()));
    REQUIRE(submitRes);
    CHECK(submitRes->status == rt::AuthoringStatus::Stale);
    CHECK(fixture.title(fixture.trackIds().front()) == "Newer");
    CHECK(form.buildPatch().optTitle == "Stale draft");
    CHECK(session.boundRevision() == revision + 1);
    CHECK(invalidatedCount == 1);
  }

  TEST_CASE(
    "TrackAuthoringSession snapshot - a commit during baseline preparation keeps coherent old facts but is stale",
    "[uimodel][integration][library-authoring][concurrency]")
  {
    auto fixture = TrackAuthoringFixture{2};
    auto const targetIds = std::array{fixture.trackIds().front()};
    auto form = TrackPropertiesFormModel{ao::test::englishMessageCatalog()};
    auto const spec = buildTrackPropertiesFormSpec(ao::test::englishMessageCatalog());
    auto session = [&]
    {
      auto snapshot = fixture.library().snapshot();
      auto sessionRes = TrackAuthoringSession::begin(fixture.library(), targetIds, snapshot);
      REQUIRE(sessionRes);
      auto otherRes = TrackAuthoringSession::begin(fixture.library(), std::array{fixture.trackIds().back()});
      REQUIRE(otherRes);
      auto const submittedRes = fixture.runTask(otherRes->submitMetadataAsync(rt::MetadataPatch{.optTitle = "Other"}));
      REQUIRE(submittedRes);
      REQUIRE(submittedRes->status == rt::AuthoringStatus::Applied);

      // A worker commit may occur between binding and baseline reads. The
      // snapshot still supplies one coherent revision, never a newer baseline
      // paired with the old binding; the final current check must reject it.
      REQUIRE(loadTrackPropertiesFormBaseline(snapshot, sessionRes->targetIds(), spec, form));
      CHECK(sessionRes->boundRevision() == snapshot.revision());
      CHECK(form.rowView(rt::TrackField::Title).text == "Old Title");
      CHECK_FALSE(sessionRes->isCurrent());
      return std::move(*sessionRes);
    }();

    CHECK_FALSE(session.isCurrent());
    auto const submittedRes = fixture.runTask(session.submitMetadataAsync(rt::MetadataPatch{.optTitle = "Draft"}));
    REQUIRE(submittedRes);
    CHECK(submittedRes->status == rt::AuthoringStatus::Stale);
    CHECK(fixture.title(targetIds.front()) == "Old Title");
    CHECK(fixture.title(fixture.trackIds().back()) == "Other");
  }

  TEST_CASE("TrackAuthoringSession snapshot - rejects foreign storage rather than binding the same local id",
            "[uimodel][unit][library-authoring]")
  {
    auto fixture = TrackAuthoringFixture{1};
    auto foreign = TrackAuthoringFixture{1};
    auto snapshot = foreign.library().snapshot();
    REQUIRE(fixture.trackIds().front() == foreign.trackIds().front());
    REQUIRE(snapshot.revision() == fixture.library().authoringAvailability().libraryRevision);

    auto const sessionRes = TrackAuthoringSession::begin(fixture.library(), fixture.trackIds(), snapshot);

    REQUIRE_FALSE(sessionRes);
    CHECK(sessionRes.error().code == Error::Code::InvalidInput);
    CHECK(fixture.title(fixture.trackIds().front()) == "Old Title");
  }

  TEST_CASE("TrackAuthoringSession snapshot - retained old snapshot never silently rebinds after a commit",
            "[uimodel][integration][library-authoring]")
  {
    auto fixture = TrackAuthoringFixture{1};
    auto snapshot = fixture.library().snapshot();
    auto const revision = snapshot.revision();
    auto sessionRes = TrackAuthoringSession::begin(fixture.library(), fixture.trackIds(), snapshot);
    REQUIRE(sessionRes);
    auto const submitRes = fixture.runTask(sessionRes->submitMetadataAsync(rt::MetadataPatch{.optTitle = "Newer"}));
    REQUIRE(submitRes);
    CHECK(submitRes->status == rt::AuthoringStatus::Applied);
    auto const optOldRow = snapshot.trackRow(fixture.trackIds().front());
    REQUIRE(optOldRow);
    CHECK(optOldRow->title == "Old Title");
    CHECK(snapshot.revision() == revision);

    auto const rejectedRes = TrackAuthoringSession::begin(fixture.library(), fixture.trackIds(), snapshot);

    REQUIRE_FALSE(rejectedRes);
    CHECK(rejectedRes.error().code == Error::Code::InvalidState);
    CHECK(sessionRes->isCurrent());
    CHECK(sessionRes->boundRevision() == revision + 1);
    CHECK(fixture.title(fixture.trackIds().front()) == "Newer");
  }
} // namespace ao::uimodel::test
