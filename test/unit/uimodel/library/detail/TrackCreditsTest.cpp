// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/uimodel/library/detail/TrackCredits.h>

#include "lib/library/PhysicalStoreAccess.h"
#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/library/Credits.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibrarySnapshot.h>
#include <ao/uimodel/library/property/TrackPropertiesFormModel.h>
#include <ao/uimodel/library/property/TrackPropertiesFormSpec.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ao::uimodel::test
{
  namespace
  {
    TrackCreditSections commonCredits(std::vector<library::Credit> const& entries = {})
    {
      auto sections = TrackCreditSections{};

      for (auto& section : sections)
      {
        section.optValue.emplace();
      }

      for (auto const& entry : entries)
      {
        sections[static_cast<std::size_t>(entry.kind)].optValue->push_back(entry);
      }

      return sections;
    }
  } // namespace

  TEST_CASE("TrackCredits - per-kind display preserves duplicates roles and unrelated mixed sections",
            "[uimodel][unit][track-credits]")
  {
    using K = library::CreditKind;
    auto sections =
      commonCredits({{"A", K::Conductor, "Guest"}, {"A", K::Conductor, ""}, {"Solo", K::Soloist, "Violin"}});
    sections[3] = {.mixed = true};
    auto const& catalog = ao::test::englishMessageCatalog();
    auto const rows = formatTrackCreditDisplayRows(catalog, sections);
    REQUIRE(rows.size() == 4);
    CHECK(rows[0].kindLabel == "Conductor");
    CHECK(rows[0].name == "A");
    CHECK(rows[0].role == "Guest");
    CHECK(rows[1].name == "A");
    CHECK(rows[1].role.empty());
    CHECK(rows[2].kind == K::Soloist);
    CHECK(rows[2].name == "Solo");
    CHECK(rows[2].role == "Violin");
    CHECK(rows[3].mixed);
    CHECK(rows[3].kindLabel == "Performer");
    CHECK(rows[3].name == "<Multiple Values>");
    CHECK(formatTrackCreditSummary(catalog, "A", 2) == "A +1");
    CHECK(formatTrackCreditSummary(catalog, "A", 1) == "A");
    CHECK(formatTrackCreditSummary(catalog, "", 0).empty());
    CHECK(formatTrackCreditDisplayRows(catalog, commonCredits()).empty());
  }

  TEST_CASE("TrackCredits - scope and section labels distinguish empty common and mixed values",
            "[uimodel][unit][track-credits]")
  {
    using K = library::CreditKind;
    auto const& catalog = ao::test::englishMessageCatalog();
    auto sections = commonCredits({{"A", K::Conductor, "Guest"}, {"A", K::Conductor, ""}});
    sections[3] = {.mixed = true};
    CHECK(trackCreditScopeLabel(catalog, {}).empty());
    CHECK(trackCreditScopeLabel(catalog, allTrackCreditKinds()) == "All credit kinds");
    CHECK(trackCreditScopeLabel(catalog, trackCreditScope(K::Conductor)) == "Conductor");
    CHECK(trackCreditScopeLabel(catalog, trackCreditScope(K::Performer)) == "Performer");
    CHECK(trackCreditScopeLabel(catalog, trackCreditScope(K::Soloist) | trackCreditScope(K::Ensemble)) ==
          "Ensemble; Soloist");
    CHECK(formatTrackCreditSectionSummary(catalog, sections[0]) == "A +1");
    CHECK(formatTrackCreditSectionSummary(catalog, sections[1]).empty());
    CHECK(formatTrackCreditSectionSummary(catalog, sections[3]) == "<Multiple Values>");
    CHECK(formatTrackCreditSectionSummary(catalog, {}).empty());
  }

  TEST_CASE("TrackCredits - validation messages localize every reason and the actual one-based row",
            "[uimodel][unit][track-credits]")
  {
    struct LocaleCase final
    {
      std::string_view locale;
      std::array<std::string_view, 4> messages;
    };

    auto const cases = std::to_array<LocaleCase>({
      {"en",
       {"Row 2: enter a name.",
        "Row 3: the name contains invalid text.",
        "Row 4: the role or instrument contains invalid text.",
        "Row 5: choose a valid credit kind for this scope."}},
      {"de",
       {"Zeile 2: Geben Sie einen Namen ein.",
        "Zeile 3: Der Name enthält ungültigen Text.",
        "Zeile 4: Die Rolle oder das Instrument enthält ungültigen Text.",
        "Zeile 5: Wählen Sie eine gültige Mitwirkungskategorie für diesen Bereich."}},
      {"es",
       {"Fila 2: introduzca un nombre.",
        "Fila 3: el nombre contiene texto no válido.",
        "Fila 4: la función o el instrumento contiene texto no válido.",
        "Fila 5: elija una categoría de crédito válida para este ámbito."}},
      {"fr",
       {"Ligne 2 : saisissez un nom.",
        "Ligne 3 : le nom contient du texte non valide.",
        "Ligne 4 : le rôle ou l’instrument contient du texte non valide.",
        "Ligne 5 : choisissez une catégorie de crédit valide pour ce périmètre."}},
      {"ja",
       {"2 行目: 名前を入力してください。",
        "3 行目: 名前に無効なテキストが含まれています。",
        "4 行目: 役割または楽器に無効なテキストが含まれています。",
        "5 行目: この範囲で有効なクレジット分類を選択してください。"}},
      {"zh-Hans",
       {"第 2 行：请输入名称。",
        "第 3 行：名称包含无效文本。",
        "第 4 行：角色或乐器包含无效文本。",
        "第 5 行：请选择此范围内的有效署名类别。"}},
      {"zh-Hant",
       {"第 2 行：請輸入名稱。",
        "第 3 行：名稱包含無效文字。",
        "第 4 行：角色或樂器包含無效文字。",
        "第 5 行：請選擇此範圍內的有效署名類別。"}},
    });
    auto const reasons = std::to_array<TrackCreditValidationReason>({TrackCreditValidationReason::BlankName,
                                                                     TrackCreditValidationReason::InvalidNameText,
                                                                     TrackCreditValidationReason::InvalidRoleText,
                                                                     TrackCreditValidationReason::InvalidKind});

    for (auto const& localeCase : cases)
    {
      CAPTURE(localeCase.locale);
      auto const catalog = ao::test::messageCatalog(localeCase.locale);

      for (std::size_t i = 0; i < reasons.size(); ++i)
      {
        CAPTURE(i);
        auto const message = formatTrackCreditValidationError(catalog, {.rowIndex = i + 1, .reason = reasons[i]});
        CHECK(message == localeCase.messages[i]);
        CHECK_FALSE(message.contains("entry 0"));
        CHECK_FALSE(message.contains("UTF-8"));
      }
    }
  }

  TEST_CASE("TrackCredits - mixed editing requires explicit replacement or a distinct explicit clear",
            "[uimodel][unit][track-credits]")
  {
    using K = library::CreditKind;
    auto sections = commonCredits({{"Must not seed", K::Conductor, "Guest"}});
    sections[3] = {.mixed = true};
    auto model = TrackCreditsEditorModel{};
    REQUIRE(model.begin(sections, allTrackCreditKinds()));
    CHECK(model.isMixedReplacement());
    CHECK(model.entries().empty());
    CHECK_FALSE(model.canEdit());
    CHECK_FALSE(model.canCommit());
    CHECK_FALSE(model.buildCommitPatch());
    model.addEntry(K::Performer);
    CHECK(model.entries().empty());

    SECTION("replacement cannot silently turn an empty mixed draft into clear")
    {
      model.beginReplacement();
      CHECK(model.canEdit());
      CHECK_FALSE(model.canCommit());
      model.addEntry(K::Performer);
      model.updateName(0, "New");
      REQUIRE(model.canCommit());
      model.deleteEntry(0);
      CHECK_FALSE(model.canCommit());
      CHECK_FALSE(model.buildCommitPatch());
      model.cancel();
      CHECK_FALSE(model.buildCommitPatch());
    }

    SECTION("clear has explicit intent and typed scope")
    {
      model.clearScope();
      auto patchRes = model.buildCommitPatch();
      REQUIRE(patchRes);
      REQUIRE(patchRes->optCredits);
      CHECK(patchRes->optCredits->kinds.all());
      CHECK(patchRes->optCredits->entries.empty());
      CHECK_FALSE(patchRes->optRecordingDate);
    }
  }

  TEST_CASE("TrackCredits - mixed clear intent survives add and delete only within its session",
            "[uimodel][unit][track-credits]")
  {
    using K = library::CreditKind;
    auto sections = commonCredits({{"Common", K::Conductor, "Guest"}});
    sections[3] = {.mixed = true};
    auto model = TrackCreditsEditorModel{};
    REQUIRE(model.begin(sections, allTrackCreditKinds()));
    model.clearScope();
    model.addEntry(K::Soloist);
    CHECK_FALSE(model.canCommit());
    model.updateName(0, "Temporary");
    model.updateRole(0, "Violin");
    REQUIRE(model.canCommit());
    model.deleteEntry(0);
    CHECK(model.canCommit());
    CHECK_FALSE(model.focusedRow());
    auto const patchRes = model.buildCommitPatch();
    REQUIRE(patchRes);
    REQUIRE(patchRes->optCredits);
    CHECK(patchRes->optCredits->kinds.all());
    CHECK(patchRes->optCredits->entries.empty());
    CHECK_FALSE(patchRes->optRecordingDate);
    model.cancel();
    REQUIRE(model.begin(sections, allTrackCreditKinds()));
    model.beginReplacement();
    model.addEntry(K::Soloist);
    model.updateName(0, "Temporary");
    model.deleteEntry(0);
    CHECK_FALSE(model.canCommit());
    CHECK_FALSE(model.buildCommitPatch());
  }

  TEST_CASE("TrackCredits - common clear roundtrip retains empty NoOp and nonempty clear semantics",
            "[uimodel][unit][track-credits]")
  {
    using K = library::CreditKind;
    auto entries = std::vector<library::Credit>{};

    SECTION("empty baseline")
    {
      entries.clear();
    }

    SECTION("nonempty baseline")
    {
      entries = {{"Original", K::Soloist, "Violin"}};
    }

    auto model = TrackCreditsEditorModel{};
    REQUIRE(model.begin(commonCredits(entries), trackCreditScope(K::Soloist)));
    model.clearScope();
    model.addEntry(K::Soloist);
    model.updateName(0, "Temporary");
    model.deleteEntry(0);
    CHECK(model.canCommit());
    auto const patchRes = model.buildCommitPatch();
    REQUIRE(patchRes);
    CHECK(patchRes->optCredits.has_value() == !entries.empty());

    if (patchRes->optCredits)
    {
      CHECK(patchRes->optCredits->kinds == trackCreditScope(K::Soloist));
      CHECK(patchRes->optCredits->entries.empty());
    }
  }

  TEST_CASE("TrackCredits - Properties full clear roundtrip replaces staged scopes without early writes",
            "[uimodel][unit][track-credits]")
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
    auto form = TrackPropertiesFormModel{ao::test::englishMessageCatalog()};
    auto const spec = buildTrackPropertiesFormSpec(ao::test::englishMessageCatalog());
    REQUIRE(loadTrackPropertiesFormBaseline(commands.library().snapshot(), std::array{first, second}, spec, form));
    form.setEditValue(rt::TrackField::Title, std::string{"After"});
    REQUIRE(form.beginCreditsEdit(trackCreditScope(K::Conductor)));
    form.creditsEditor().updateName(0, "Staged");
    REQUIRE(form.acceptCreditsEdit());
    REQUIRE(form.beginCreditsEdit(allTrackCreditKinds()));
    REQUIRE(form.creditsEditor().isMixedReplacement());
    form.creditsEditor().clearScope();
    form.creditsEditor().addEntry(K::Soloist);
    form.creditsEditor().updateName(0, "Temporary");
    form.creditsEditor().deleteEntry(0);
    CHECK_FALSE(form.canSave());
    CHECK_FALSE(form.buildPatch().optCredits);
    CHECK_FALSE(form.buildPatch().optTitle);
    REQUIRE(form.acceptCreditsEdit());
    CHECK(form.canSave());
    auto const patch = form.buildPatch();
    CHECK(patch.optTitle == "After");
    REQUIRE(patch.optCredits);
    CHECK(patch.optCredits->kinds.all());
    CHECK(patch.optCredits->entries.empty());
    auto const snapshot = commands.library().snapshot();
    CHECK(snapshot.trackCredits(first) ==
          std::vector<library::Credit>{{"A", K::Conductor, "Guest"}, {"P1", K::Performer, "Piano"}});
    CHECK(snapshot.trackCredits(second) ==
          std::vector<library::Credit>{{"A", K::Conductor, "Guest"}, {"P2", K::Performer, "Voice"}});
    CHECK(std::get<std::string>(snapshot.trackField(first, rt::TrackField::Title)) == "Before");
  }

  TEST_CASE("TrackCredits - failed validation retains row text attributes errors and focus",
            "[uimodel][unit][track-credits]")
  {
    using K = library::CreditKind;
    auto model = TrackCreditsEditorModel{};
    REQUIRE(model.begin(commonCredits({{"A", K::Soloist, "Violin"}}), trackCreditScope(K::Soloist)));
    model.updateName(0, " \t\r\n");
    auto const errors = model.validationErrors();
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].rowIndex == 0);
    CHECK(errors[0].reason == TrackCreditValidationReason::BlankName);
    CHECK_FALSE(model.canCommit());
    CHECK_FALSE(model.buildCommitPatch());
    CHECK(model.focusedRow() == 0);
    CHECK(model.entries() == std::vector<library::Credit>{{" \t\r\n", K::Soloist, "Violin"}});
    model.updateName(0, " Gould ");
    auto patchRes = model.buildCommitPatch();
    REQUIRE(patchRes);
    REQUIRE(patchRes->optCredits);
    CHECK(patchRes->optCredits->entries == std::vector<library::Credit>{{"Gould", K::Soloist, "Violin"}});
    CHECK(model.validationErrors().empty());
    CHECK(model.entries()[0].name == " Gould ");
  }

  TEST_CASE("TrackCredits - later invalid rows report typed reasons without changing the draft",
            "[uimodel][unit][track-credits]")
  {
    using K = library::CreditKind;
    auto model = TrackCreditsEditorModel{};
    REQUIRE(model.begin(commonCredits({{"First", K::Performer, ""},
                                       {"Second", K::Performer, ""},
                                       {"Third", K::Performer, ""},
                                       {"Fourth", K::Performer, ""}}),
                        trackCreditScope(K::Performer)));
    model.updateRole(0, " \t\r\n\v\f");
    model.updateName(1, " \t\r\n\v\f");
    model.updateName(2, "bad\xff");
    model.updateRole(3, "bad\xed\xa0\x80");
    auto const draft = model.entries();
    auto const errors = model.validationErrors();
    REQUIRE(errors.size() == 3);
    CHECK(errors[0].rowIndex == 1);
    CHECK(errors[0].reason == TrackCreditValidationReason::BlankName);
    CHECK(errors[1].rowIndex == 2);
    CHECK(errors[1].reason == TrackCreditValidationReason::InvalidNameText);
    CHECK(errors[2].rowIndex == 3);
    CHECK(errors[2].reason == TrackCreditValidationReason::InvalidRoleText);
    CHECK_FALSE(model.canCommit());
    CHECK_FALSE(model.buildCommitPatch());
    CHECK(model.entries() == draft);
    CHECK(model.focusedRow() == 3);
    CHECK(model.isEditing());

    model.updateName(1, "Second");
    model.updateName(2, "Third");
    model.updateRole(3, "Violin");
    CHECK(model.validationErrors().empty());
    auto const patchRes = model.buildCommitPatch();
    REQUIRE(patchRes);
    REQUIRE(patchRes->optCredits);
    CHECK(patchRes->optCredits->entries == std::vector<library::Credit>{{"First", K::Performer, ""},
                                                                        {"Second", K::Performer, ""},
                                                                        {"Third", K::Performer, ""},
                                                                        {"Fourth", K::Performer, "Violin"}});
  }

  TEST_CASE("TrackCredits - public baselines reject invalid kinds and noncanonical text without opening",
            "[uimodel][unit][track-credits]")
  {
    using K = library::CreditKind;
    auto sections = commonCredits({{"A", K::Conductor, "Guest"}});
    auto& entry = sections[0].optValue->front();

    SECTION("unknown kind")
    {
      entry.kind = static_cast<K>(255);
    }

    SECTION("wrong section")
    {
      entry.kind = K::Soloist;
    }

    SECTION("blank name")
    {
      entry.name = " \t";
    }

    SECTION("untrimmed name")
    {
      entry.name = " A ";
    }

    SECTION("untrimmed role")
    {
      entry.role = " Guest ";
    }

    SECTION("invalid name")
    {
      entry.name = "bad\xff";
    }

    SECTION("invalid role")
    {
      entry.role = "bad\xff";
    }

    SECTION("noncanonical name")
    {
      entry.name = "e\xcc\x81";
    }

    SECTION("noncanonical role")
    {
      entry.role = "e\xcc\x81";
    }

    auto model = TrackCreditsEditorModel{};
    CHECK_FALSE(model.begin(sections, trackCreditScope(K::Conductor)));
    CHECK_FALSE(model.isEditing());
    CHECK(model.entries().empty());
    CHECK_FALSE(undoValueForClearedTrackCredits(sections, trackCreditScope(K::Conductor)));
  }

  TEST_CASE("TrackCredits - complete normalized equality includes role multiplicity and within-kind order",
            "[uimodel][unit][track-credits]")
  {
    using K = library::CreditKind;
    auto model = TrackCreditsEditorModel{};
    REQUIRE(model.begin(
      commonCredits({{"\xc3\xa9", K::Conductor, "Guest"}, {"B", K::Conductor, ""}, {"B", K::Conductor, ""}}),
      trackCreditScope(K::Conductor)));
    model.updateName(0, " e\xcc\x81 ");
    model.updateRole(0, " Guest \t");
    auto patchRes = model.buildCommitPatch();
    REQUIRE(patchRes);
    CHECK_FALSE(patchRes->optCredits);
    model.moveEntry(0, 2);
    patchRes = model.buildCommitPatch();
    REQUIRE(patchRes);
    REQUIRE(patchRes->optCredits);
    CHECK(patchRes->optCredits->entries == std::vector<library::Credit>{{"B", K::Conductor, ""},
                                                                        {"B", K::Conductor, ""},
                                                                        {"\xc3\xa9", K::Conductor, "Guest"}});
    model.deleteEntry(0);
    model.updateRole(1, "New role");
    patchRes = model.buildCommitPatch();
    REQUIRE(patchRes);
    REQUIRE(patchRes->optCredits);
    CHECK(patchRes->optCredits->entries ==
          std::vector<library::Credit>{{"B", K::Conductor, ""}, {"\xc3\xa9", K::Conductor, "New role"}});
  }

  TEST_CASE("TrackCredits - full editor reclassification appends destination and forbids cross-kind reorder",
            "[uimodel][unit][track-credits]")
  {
    using K = library::CreditKind;
    auto const sections =
      commonCredits({{"A", K::Conductor, "Guest"}, {"B", K::Soloist, "Violin"}, {"C", K::Soloist, "Cello"}});
    auto model = TrackCreditsEditorModel{};
    REQUIRE(model.begin(sections, allTrackCreditKinds()));
    model.addEntry(static_cast<K>(255));
    model.changeKind(0, static_cast<K>(255));
    CHECK(model.entries() == std::vector<library::Credit>{
                               {"A", K::Conductor, "Guest"}, {"B", K::Soloist, "Violin"}, {"C", K::Soloist, "Cello"}});
    model.moveEntry(0, 2);
    CHECK(model.entries()[0].name == "A");
    model.changeKind(0, K::Soloist);
    CHECK(model.entries() == std::vector<library::Credit>{
                               {"B", K::Soloist, "Violin"}, {"C", K::Soloist, "Cello"}, {"A", K::Soloist, "Guest"}});
    CHECK(model.focusedRow() == 2);
    model.changeKind(2, K::Ensemble);
    CHECK(model.entries() == std::vector<library::Credit>{
                               {"A", K::Ensemble, "Guest"}, {"B", K::Soloist, "Violin"}, {"C", K::Soloist, "Cello"}});
    model.moveEntry(2, 1);
    CHECK(model.entries()[1].name == "C");
    CHECK(model.entries()[1].role == "Cello");
    model.cancel();
    REQUIRE(model.begin(sections, trackCreditScope(K::Conductor)));
    model.changeKind(0, K::Soloist);
    model.addEntry(K::Soloist);
    CHECK(model.entries() == std::vector<library::Credit>{{"A", K::Conductor, "Guest"}});
    CHECK_FALSE(model.begin(sections, allTrackCreditKinds()));
    CHECK(model.scope() == trackCreditScope(K::Conductor));
  }

  TEST_CASE("TrackCredits - fixed category deletion clears only that scope and invalid scopes never open",
            "[uimodel][unit][track-credits]")
  {
    using K = library::CreditKind;
    auto const sections = commonCredits({{"A", K::Soloist, "Violin"}});
    auto model = TrackCreditsEditorModel{};
    CHECK_FALSE(model.begin(sections, {}));
    CHECK_FALSE(model.begin(sections, trackCreditScope(K::Soloist) | trackCreditScope(K::Performer)));
    CHECK_FALSE(model.isEditing());
    REQUIRE(model.begin(sections, trackCreditScope(K::Soloist)));
    model.deleteEntry(0);
    auto const patchRes = model.buildCommitPatch();
    REQUIRE(patchRes);
    REQUIRE(patchRes->optCredits);
    CHECK(patchRes->optCredits->kinds == trackCreditScope(K::Soloist));
    CHECK(patchRes->optCredits->entries.empty());
    CHECK_FALSE(patchRes->optRecordingDate);
    CHECK_FALSE(model.focusedRow());
    model.cancel();
    CHECK(model.entries().empty());
    CHECK_FALSE(model.buildCommitPatch());
  }

  TEST_CASE("TrackCredits - scoped clear undo ignores unrelated mixed categories", "[uimodel][unit][track-credits]")
  {
    using K = library::CreditKind;
    auto sections = commonCredits({{"A", K::Conductor, "Guest"}, {"A", K::Conductor, "Guest"}});
    sections[3] = {.mixed = true};
    auto const scope = trackCreditScope(K::Conductor);
    auto const optUndo = undoValueForClearedTrackCredits(sections, scope);
    REQUIRE(optUndo);
    CHECK(optUndo->kinds == scope);
    CHECK(optUndo->entries == *sections[0].optValue);
    CHECK_FALSE(undoValueForClearedTrackCredits(sections, allTrackCreditKinds()));
    CHECK_FALSE(undoValueForClearedTrackCredits(sections, trackCreditScope(K::Soloist)));
    CHECK_FALSE(undoValueForClearedTrackCredits({}, scope));
    auto model = TrackCreditsEditorModel{};
    REQUIRE(model.begin(sections, scope));
    model.clearScope();
    auto const patchRes = model.buildCommitPatch();
    REQUIRE(patchRes);
    REQUIRE(patchRes->optCredits);
    CHECK(patchRes->optCredits->kinds == scope);
    CHECK(patchRes->optCredits->entries.empty());
    CHECK_FALSE(patchRes->optRecordingDate);
    CHECK(shouldShowTrackCredits(true, false, true, sections, false));
    CHECK_FALSE(shouldShowTrackCredits(false, true, true, sections, true));
    CHECK_FALSE(shouldShowTrackCredits(true, true, false, sections, true));
    CHECK_FALSE(shouldShowTrackCredits(true, false, true, commonCredits(), false));
    CHECK(shouldShowTrackCredits(true, false, true, commonCredits(), true));
    CHECK(shouldShowTrackCredits(true, true, true, commonCredits(), false));
  }

  TEST_CASE("TrackCredits - owning baseline distinguishes missing empty and complete per-kind disagreement",
            "[uimodel][unit][track-credits]")
  {
    using K = library::CreditKind;
    auto storage = rt::test::MusicLibraryFixture{};
    auto changes = rt::test::makeStateOnlyLibraryChanges(storage.library());
    auto commands = rt::test::LibraryCommandsFixture{storage.library(), changes};
    auto const first =
      commands.addTrack({.credits = {{"A", K::Conductor, ""}, {"B", K::Performer, "Piano"}}, .uri = "first.flac"});
    auto const second =
      commands.addTrack({.credits = {{"A", K::Conductor, ""}, {"B", K::Performer, "Voice"}}, .uri = "second.flac"});
    auto const empty = commands.addTrack({.uri = "empty.flac"});
    auto const sectionsRes = [&]
    {
      auto snapshot = commands.library().snapshot();
      CHECK_FALSE(loadTrackCreditsEditorBaseline(snapshot, std::span<TrackId const>{}));
      auto const missingRes = loadTrackCreditsEditorBaseline(snapshot, std::array{first, TrackId{999999}});
      REQUIRE_FALSE(missingRes);
      CHECK(missingRes.error().code == Error::Code::NotFound);
      auto const emptyRes = loadTrackCreditsEditorBaseline(snapshot, std::array{empty});
      REQUIRE(emptyRes);
      REQUIRE((*emptyRes)[3].optValue);
      CHECK((*emptyRes)[3].optValue->empty());
      auto const repeatedRes = loadTrackCreditsEditorBaseline(snapshot, std::array{first, first});
      REQUIRE(repeatedRes);
      CHECK_FALSE((*repeatedRes)[3].mixed);
      CHECK((*repeatedRes)[3].optValue == std::vector<library::Credit>{{"B", K::Performer, "Piano"}});
      return loadTrackCreditsEditorBaseline(snapshot, std::array{first, second, first});
    }();
    REQUIRE(sectionsRes);
    CHECK((*sectionsRes)[0].optValue == std::vector<library::Credit>{{"A", K::Conductor, ""}});
    CHECK((*sectionsRes)[3].mixed);
    CHECK_FALSE((*sectionsRes)[3].optValue);
    auto model = TrackCreditsEditorModel{};
    REQUIRE(model.begin(*sectionsRes, trackCreditScope(K::Conductor)));
    CHECK_FALSE(model.isMixedReplacement());
    CHECK(model.entries() == std::vector<library::Credit>{{"A", K::Conductor, ""}});
    auto patchRes = model.buildCommitPatch();
    REQUIRE(patchRes);
    CHECK_FALSE(patchRes->optCredits);
  }

  TEST_CASE("TrackCredits - present target with missing cold record reports corrupt baseline",
            "[uimodel][unit][track-credits]")
  {
    auto storage = rt::test::MusicLibraryFixture{};
    auto const id = storage.addTrack({.title = "Present", .uri = "present.flac"});

    // Inject post-open corruption before runtime adoption claims the Core write owner.
    {
      auto transaction = library::test::writeTransaction(storage.library());
      REQUIRE(library::detail::PhysicalStoreAccess::tryRemoveColdTrackRecordForTest(
        storage.library().tracks(), transaction, id));
      REQUIRE(transaction.commit());
    }

    auto changes = rt::test::makeStateOnlyLibraryChanges(storage.library());
    auto commands = rt::test::LibraryCommandsFixture{storage.library(), changes};
    auto const snapshot = commands.library().snapshot();
    REQUIRE(snapshot.containsTrack(id));
    CHECK_FALSE(snapshot.trackCredits(id));
    auto const baselineRes = loadTrackCreditsEditorBaseline(snapshot, std::array{id});
    REQUIRE_FALSE(baselineRes);
    CHECK(baselineRes.error().code == Error::Code::CorruptData);
    CHECK(baselineRes.error().message == "Credits editing target has unreadable credits");
  }
} // namespace ao::uimodel::test
