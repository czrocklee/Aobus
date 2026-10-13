// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include "test/unit/uimodel/library/track/TrackAuthoringTestSupport.h"
#include <ao/library/Credits.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibrarySnapshot.h>
#include <ao/uimodel/library/detail/TrackCredits.h>
#include <ao/uimodel/library/property/TrackPropertiesFormModel.h>
#include <ao/uimodel/library/property/TrackPropertiesFormSpec.h>
#include <ao/uimodel/library/track/TrackAuthoringSessions.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>
#include <utility>
#include <vector>

namespace ao::uimodel::test
{
  TEST_CASE("TrackProperties Credits - sequential scopes overlay drafts and save ordinary metadata atomically",
            "[uimodel][integration][track-credits]")
  {
    using K = library::CreditKind;
    auto storage = rt::test::MusicLibraryFixture{};
    auto changes = rt::test::makeStateOnlyLibraryChanges(storage.library());
    auto commands = rt::test::LibraryCommandsFixture{storage.library(), changes};
    auto const first = commands.addTrack({.title = "Before",
                                          .credits = {{"A", K::Conductor, "Guest"}, {"P1", K::Performer, "Piano"}},
                                          .uri = "first.flac"});
    auto const second = commands.addTrack({.title = "Before",
                                           .credits = {{"A", K::Conductor, "Guest"}, {"P2", K::Performer, "Voice"}},
                                           .uri = "second.flac"});
    auto const ids = std::array{first, second, first};
    auto form = TrackPropertiesFormModel{ao::test::englishMessageCatalog()};
    auto const spec = buildTrackPropertiesFormSpec(ao::test::englishMessageCatalog());
    auto session = [&]
    {
      auto snapshot = commands.library().snapshot();
      auto sessionRes = TrackAuthoringSession::begin(commands.library(), ids, snapshot);
      REQUIRE(sessionRes);
      REQUIRE(loadTrackPropertiesFormBaseline(snapshot, ids, spec, form));
      return std::move(*sessionRes);
    }();
    auto const revision = session.boundRevision();
    CHECK(form.creditSections()[3].mixed);
    CHECK_FALSE(form.rowView(rt::TrackField::Conductor).editable);
    form.setEditValue(rt::TrackField::Title, std::string{"After"});
    REQUIRE(form.beginCreditsEdit(trackCreditScope(K::Conductor)));
    CHECK_FALSE(form.canSave());
    CHECK_FALSE(form.buildPatch().optTitle);
    CHECK_FALSE(form.beginCreditsEdit(trackCreditScope(K::Soloist)));
    form.creditsEditor().updateName(0, "New conductor");
    REQUIRE(form.acceptCreditsEdit());
    CHECK(form.canSave());
    REQUIRE(form.beginCreditsEdit(trackCreditScope(K::Soloist)));
    form.creditsEditor().addEntry(K::Soloist);
    form.creditsEditor().updateName(0, "New soloist");
    form.creditsEditor().updateRole(0, "Violin");
    REQUIRE(form.acceptCreditsEdit());
    REQUIRE(form.pendingCredits());
    CHECK(form.pendingCredits()->kinds == (trackCreditScope(K::Conductor) | trackCreditScope(K::Soloist)));
    CHECK(form.pendingCredits()->entries == std::vector<library::Credit>{{"New conductor", K::Conductor, "Guest"},
                                                                         {"New soloist", K::Soloist, "Violin"}});
    REQUIRE(form.beginCreditsEdit(trackCreditScope(K::Conductor)));
    CHECK(form.creditsEditor().entries() == std::vector<library::Credit>{{"New conductor", K::Conductor, "Guest"}});
    form.creditsEditor().clearScope();
    form.cancelCreditsEdit();
    CHECK(form.buildPatch().optTitle == "After");
    CHECK(form.creditSections()[0].optValue == std::vector<library::Credit>{{"New conductor", K::Conductor, "Guest"}});
    CHECK(form.rowView(rt::TrackField::Conductor).text == "A");
    REQUIRE(commands.library().snapshot().trackCredits(first));
    CHECK(commands.library().snapshot().trackCredits(first)->front().name == "A");
    CHECK(session.boundRevision() == revision);
    auto const submittedRes = commands.runTask(session.submitMetadataAsync(form.buildPatch()));
    REQUIRE(submittedRes);
    CHECK(submittedRes->status == rt::AuthoringStatus::Applied);
    CHECK(session.boundRevision() == revision + 1);
    auto snapshot = commands.library().snapshot();
    CHECK(snapshot.trackCredits(first) == std::vector<library::Credit>{{"New conductor", K::Conductor, "Guest"},
                                                                       {"New soloist", K::Soloist, "Violin"},
                                                                       {"P1", K::Performer, "Piano"}});
    CHECK(snapshot.trackCredits(second) == std::vector<library::Credit>{{"New conductor", K::Conductor, "Guest"},
                                                                        {"New soloist", K::Soloist, "Violin"},
                                                                        {"P2", K::Performer, "Voice"}});
    CHECK(std::get<std::string>(snapshot.trackField(first, rt::TrackField::Title)) == "After");
    CHECK(std::get<std::string>(snapshot.trackField(second, rt::TrackField::Title)) == "After");
  }

  TEST_CASE("TrackProperties Credits - invalid child rows retain staged scopes and ordinary drafts",
            "[uimodel][unit][track-credits]")
  {
    using K = library::CreditKind;
    auto storage = rt::test::MusicLibraryFixture{};
    auto changes = rt::test::makeStateOnlyLibraryChanges(storage.library());
    auto commands = rt::test::LibraryCommandsFixture{storage.library(), changes};
    auto const id = commands.addTrack({.title = "Before"});
    auto form = TrackPropertiesFormModel{ao::test::englishMessageCatalog()};
    auto const spec = buildTrackPropertiesFormSpec(ao::test::englishMessageCatalog());
    REQUIRE(loadTrackPropertiesFormBaseline(commands.library().snapshot(), std::array{id}, spec, form));
    form.setEditValue(rt::TrackField::Title, std::string{"After"});
    REQUIRE(form.beginCreditsEdit(trackCreditScope(K::Soloist)));
    form.creditsEditor().addEntry(K::Soloist);
    form.creditsEditor().updateName(0, " e\xcc\x81 ");
    form.creditsEditor().updateRole(0, " Violin ");
    REQUIRE(form.acceptCreditsEdit());
    REQUIRE(form.beginCreditsEdit(trackCreditScope(K::Conductor)));
    form.creditsEditor().addEntry(K::Conductor);
    form.creditsEditor().updateName(0, "First");
    form.creditsEditor().addEntry(K::Conductor);
    form.creditsEditor().updateName(1, "Second");
    form.creditsEditor().updateRole(1, "bad\xff");
    auto const draft = form.creditsEditor().entries();
    CHECK_FALSE(form.acceptCreditsEdit());
    CHECK_FALSE(form.canSave());
    CHECK(form.creditsEditor().isEditing());
    CHECK(form.creditsEditor().entries() == draft);
    CHECK(form.creditsEditor().focusedRow() == 1);
    auto const errors = form.creditsEditor().validationErrors();
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].rowIndex == 1);
    CHECK(errors[0].reason == TrackCreditValidationReason::InvalidRoleText);
    REQUIRE(form.pendingCredits());
    CHECK(form.pendingCredits()->entries == std::vector<library::Credit>{{"\xc3\xa9", K::Soloist, "Violin"}});
    form.creditsEditor().updateRole(1, " Guest ");
    REQUIRE(form.acceptCreditsEdit());
    auto const patch = form.buildPatch();
    CHECK(patch.optTitle == "After");
    REQUIRE(patch.optCredits);
    CHECK(patch.optCredits->entries == std::vector<library::Credit>{{"First", K::Conductor, ""},
                                                                    {"Second", K::Conductor, "Guest"},
                                                                    {"\xc3\xa9", K::Soloist, "Violin"}});
    REQUIRE(form.beginCreditsEdit(trackCreditScope(K::Soloist)));
    CHECK(form.creditsEditor().entries() == std::vector<library::Credit>{{"\xc3\xa9", K::Soloist, "Violin"}});
    form.cancelCreditsEdit();
    CHECK(form.buildPatch().optTitle == "After");
    CHECK(commands.library().snapshot().trackCredits(id) == std::vector<library::Credit>{});
  }

  TEST_CASE("TrackProperties Credits - full mixed reopen never seeds staged common subsets",
            "[uimodel][unit][track-credits]")
  {
    using K = library::CreditKind;
    auto storage = rt::test::MusicLibraryFixture{};
    auto changes = rt::test::makeStateOnlyLibraryChanges(storage.library());
    auto commands = rt::test::LibraryCommandsFixture{storage.library(), changes};
    auto const first =
      commands.addTrack({.credits = {{"A", K::Conductor, ""}, {"P1", K::Performer, ""}}, .uri = "first.flac"});
    auto const second =
      commands.addTrack({.credits = {{"A", K::Conductor, ""}, {"P2", K::Performer, ""}}, .uri = "second.flac"});
    auto form = TrackPropertiesFormModel{ao::test::englishMessageCatalog()};
    auto const spec = buildTrackPropertiesFormSpec(ao::test::englishMessageCatalog());
    REQUIRE(loadTrackPropertiesFormBaseline(commands.library().snapshot(), std::array{first, second}, spec, form));
    REQUIRE(form.beginCreditsEdit(trackCreditScope(K::Conductor)));
    form.creditsEditor().updateName(0, "Staged");
    REQUIRE(form.acceptCreditsEdit());
    REQUIRE(form.beginCreditsEdit(allTrackCreditKinds()));
    CHECK(form.creditsEditor().isMixedReplacement());
    CHECK(form.creditsEditor().entries().empty());
    CHECK_FALSE(form.acceptCreditsEdit());
    CHECK_FALSE(form.canSave());
    CHECK(form.creditsEditor().isEditing());
    form.cancelCreditsEdit();
    REQUIRE(form.pendingCredits());
    CHECK(form.pendingCredits()->entries == std::vector<library::Credit>{{"Staged", K::Conductor, ""}});
    REQUIRE(form.beginCreditsEdit(allTrackCreditKinds()));
    form.creditsEditor().beginReplacement();
    form.creditsEditor().addEntry(K::Ensemble);
    form.creditsEditor().updateName(0, "Entire replacement");
    REQUIRE(form.acceptCreditsEdit());
    REQUIRE(form.pendingCredits());
    CHECK(form.pendingCredits()->kinds.all());
    CHECK(form.pendingCredits()->entries == std::vector<library::Credit>{{"Entire replacement", K::Ensemble, ""}});
    REQUIRE(form.beginCreditsEdit(trackCreditScope(K::Conductor)));
    CHECK_FALSE(form.creditsEditor().isMixedReplacement());
    CHECK(form.creditsEditor().entries().empty());
    form.cancelCreditsEdit();
  }

  TEST_CASE(
    "TrackProperties Credits - reverting normalized scope removes pending patch and scalar writes stay disabled",
    "[uimodel][unit][track-credits]")
  {
    using K = library::CreditKind;
    auto storage = rt::test::MusicLibraryFixture{};
    auto changes = rt::test::makeStateOnlyLibraryChanges(storage.library());
    auto commands = rt::test::LibraryCommandsFixture{storage.library(), changes};
    auto const id = commands.addTrack({.credits = {{"A", K::Conductor, "Role"}, {"A", K::Conductor, "Role"}}});
    auto form = TrackPropertiesFormModel{ao::test::englishMessageCatalog()};
    auto const spec = buildTrackPropertiesFormSpec(ao::test::englishMessageCatalog());
    REQUIRE(loadTrackPropertiesFormBaseline(commands.library().snapshot(), std::array{id}, spec, form));
    CHECK(form.rowView(rt::TrackField::Conductor).text == "A +1");
    form.setExplicitFieldEdit(rt::TrackField::Conductor, std::string{"Cannot overwrite hidden role"});
    CHECK_FALSE(form.canSave());
    CHECK_FALSE(form.buildPatch().optCredits);
    REQUIRE(form.beginCreditsEdit(trackCreditScope(K::Conductor)));
    form.creditsEditor().clearScope();
    REQUIRE(form.acceptCreditsEdit());
    REQUIRE(form.pendingCredits());
    CHECK(form.pendingCredits()->entries.empty());
    REQUIRE(form.beginCreditsEdit(trackCreditScope(K::Conductor)));
    form.creditsEditor().addEntry(K::Conductor);
    form.creditsEditor().updateName(0, " A ");
    form.creditsEditor().updateRole(0, " Role ");
    form.creditsEditor().addEntry(K::Conductor);
    form.creditsEditor().updateName(1, "A");
    form.creditsEditor().updateRole(1, "Role");
    REQUIRE(form.acceptCreditsEdit());
    CHECK_FALSE(form.pendingCredits());
    CHECK_FALSE(form.canSave());
  }

  TEST_CASE("TrackProperties Credits - unchanged primary does not hide mixed complete category values",
            "[uimodel][unit][track-credits]")
  {
    using K = library::CreditKind;
    auto storage = rt::test::MusicLibraryFixture{};
    auto changes = rt::test::makeStateOnlyLibraryChanges(storage.library());
    auto commands = rt::test::LibraryCommandsFixture{storage.library(), changes};
    auto const first =
      commands.addTrack({.credits = {{"A", K::Conductor, ""}, {"B", K::Conductor, ""}}, .uri = "first.flac"});
    auto const second =
      commands.addTrack({.credits = {{"A", K::Conductor, ""}, {"C", K::Conductor, ""}}, .uri = "second.flac"});
    auto form = TrackPropertiesFormModel{ao::test::englishMessageCatalog()};
    auto const spec = buildTrackPropertiesFormSpec(ao::test::englishMessageCatalog());
    REQUIRE(loadTrackPropertiesFormBaseline(commands.library().snapshot(), std::array{first, second}, spec, form));
    CHECK(form.rowView(rt::TrackField::Conductor).mixed);
    CHECK(form.rowView(rt::TrackField::Conductor).text == "<Multiple Values>");
    REQUIRE(form.beginCreditsEdit(trackCreditScope(K::Conductor)));
    CHECK(form.creditsEditor().entries().empty());
    CHECK_FALSE(form.creditsEditor().canEdit());
    form.creditsEditor().clearScope();
    REQUIRE(form.acceptCreditsEdit());
    REQUIRE(form.pendingCredits());
    CHECK(form.pendingCredits()->kinds == trackCreditScope(K::Conductor));
    CHECK(form.pendingCredits()->entries.empty());
  }

  TEST_CASE("TrackProperties Credits - stale submission retains draft and reopening retains captured other scopes",
            "[uimodel][integration][track-credits]")
  {
    using K = library::CreditKind;
    auto fixture = TrackAuthoringFixture{1};
    auto form = TrackPropertiesFormModel{ao::test::englishMessageCatalog()};
    auto const spec = buildTrackPropertiesFormSpec(ao::test::englishMessageCatalog());
    auto session = [&]
    {
      auto snapshot = fixture.library().snapshot();
      auto sessionRes = TrackAuthoringSession::begin(fixture.library(), fixture.trackIds(), snapshot);
      REQUIRE(sessionRes);
      REQUIRE(loadTrackPropertiesFormBaseline(snapshot, fixture.trackIds(), spec, form));
      return std::move(*sessionRes);
    }();
    REQUIRE(form.beginCreditsEdit(trackCreditScope(K::Conductor)));
    form.creditsEditor().addEntry(K::Conductor);
    form.creditsEditor().updateName(0, "Draft");
    REQUIRE(form.acceptCreditsEdit());
    form.setEditValue(rt::TrackField::Title, std::string{"Draft title"});
    auto otherRes = TrackAuthoringSession::begin(fixture.library(), fixture.trackIds());
    REQUIRE(otherRes);
    auto const otherSubmitRes = fixture.runTask(otherRes->submitMetadataAsync(
      rt::MetadataPatch{.optCredits = rt::CreditReplacement{
                          .kinds = trackCreditScope(K::Performer), .entries = {{"Newer", K::Performer, "Piano"}}}}));
    REQUIRE(otherSubmitRes);
    REQUIRE(otherSubmitRes->status == rt::AuthoringStatus::Applied);
    CHECK_FALSE(session.isCurrent());
    auto const submittedRes = fixture.runTask(session.submitMetadataAsync(form.buildPatch()));
    REQUIRE(submittedRes);
    CHECK(submittedRes->status == rt::AuthoringStatus::Stale);
    CHECK(form.buildPatch().optTitle == "Draft title");
    REQUIRE(form.pendingCredits());
    CHECK(form.pendingCredits()->entries == std::vector<library::Credit>{{"Draft", K::Conductor, ""}});
    REQUIRE(form.beginCreditsEdit(trackCreditScope(K::Performer)));
    CHECK(form.creditsEditor().entries().empty());
    form.cancelCreditsEdit();
    CHECK(fixture.title(fixture.trackIds().front()) == "Old Title");
    CHECK(fixture.library().snapshot().trackCredits(fixture.trackIds().front()) ==
          std::vector<library::Credit>{{"Newer", K::Performer, "Piano"}});
  }
} // namespace ao::uimodel::test
