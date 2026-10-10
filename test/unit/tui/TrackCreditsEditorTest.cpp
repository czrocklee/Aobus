// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "tui/TrackCreditsEditor.h"

#include "RenderTestSupport.h"
#include "test/unit/MessageCatalogTestSupport.h"
#include <ao/library/Credits.h>
#include <ao/rt/completion/CompletionItem.h>
#include <ao/rt/completion/CompletionResult.h>
#include <ao/uimodel/library/detail/TrackCredits.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/screen/screen.hpp>

#include <cstddef>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::tui::test
{
  namespace
  {
    uimodel::TrackCreditSections emptySections()
    {
      auto sections = uimodel::TrackCreditSections{};

      for (auto& section : sections)
      {
        section.optValue.emplace();
      }

      return sections;
    }

    void tab(TrackCreditsEditor& editor, uimodel::TrackCreditsEditorModel& model, std::int32_t const count)
    {
      for (std::int32_t index = 0; index < count; ++index)
      {
        CHECK(editor.handleEvent(ftxui::Event::Tab, model) == TrackCreditsEditor::Request::None);
      }
    }
  } // namespace

  TEST_CASE("TrackCreditsEditor - keyboard edits names and roles without losing duplicate entries",
            "[tui][unit][credits]")
  {
    using library::CreditKind;
    auto sections = emptySections();
    sections[0].optValue =
      std::vector<library::Credit>{{"A", CreditKind::Conductor, "guest"}, {"A", CreditKind::Conductor, "guest"}};
    auto model = uimodel::TrackCreditsEditorModel{};
    REQUIRE(model.begin(sections, uimodel::trackCreditScope(CreditKind::Conductor)));
    auto editor = TrackCreditsEditor{ao::test::englishMessageCatalog()};
    editor.reset(model);
    editor.handleEvent(ftxui::Event::End, model);
    editor.handleEvent(ftxui::Event::Character("B"), model);
    tab(editor, model, 1);
    editor.handleEvent(ftxui::Event::End, model);
    editor.handleEvent(ftxui::Event::Character(" role"), model);
    tab(editor, model, 1);
    editor.handleEvent(ftxui::Event::Return, model); // Locked category.
    CHECK(model.entries()[0] == library::Credit{"AB", CreditKind::Conductor, "guest role"});
    CHECK(model.entries()[1] == library::Credit{"A", CreditKind::Conductor, "guest"});
    CHECK(editor.handleEvent(ftxui::Event::CtrlS, model) == TrackCreditsEditor::Request::Accept);
    auto patchRes = model.buildCommitPatch();
    REQUIRE(patchRes);
    REQUIRE(patchRes->optCredits);
    CHECK(patchRes->optCredits->kinds == uimodel::trackCreditScope(CreditKind::Conductor));
    CHECK(patchRes->optCredits->entries == model.entries());
  }

  TEST_CASE("TrackCreditsEditor - keyboard add validation reorder delete and reclassification retain attributes",
            "[tui][unit][credits]")
  {
    using library::CreditKind;
    auto sections = emptySections();
    sections[0].optValue =
      std::vector<library::Credit>{{"A", CreditKind::Conductor, "guest"}, {"B", CreditKind::Conductor, "second"}};
    sections[1].optValue = std::vector<library::Credit>{{"Group", CreditKind::Ensemble, ""}};
    auto model = uimodel::TrackCreditsEditorModel{};
    REQUIRE(model.begin(sections, uimodel::allTrackCreditKinds()));
    auto editor = TrackCreditsEditor{ao::test::englishMessageCatalog()};
    editor.reset(model);
    tab(editor, model, 6); // Move Down.
    editor.handleEvent(ftxui::Event::Return, model);
    CHECK(model.entries()[0].name == "B");
    CHECK(model.entries()[1] == library::Credit{"A", CreditKind::Conductor, "guest"});
    editor.reset(model);
    tab(editor, model, 2); // Kind.
    editor.handleEvent(ftxui::Event::Return, model);
    CHECK(model.entries() == std::vector<library::Credit>{{"A", CreditKind::Conductor, "guest"},
                                                          {"Group", CreditKind::Ensemble, ""},
                                                          {"B", CreditKind::Ensemble, "second"}});
    tab(editor, model, 1); // Add; focuses new Name.
    editor.handleEvent(ftxui::Event::Return, model);
    REQUIRE(model.entries().size() == 4);
    CHECK(editor.handleEvent(ftxui::Event::CtrlS, model) == TrackCreditsEditor::Request::None);
    CHECK(model.focusedRow() == 3);
    CHECK_FALSE(model.validationErrors().empty());
    editor.handleEvent(ftxui::Event::Character("New"), model);
    CHECK(model.entries()[3].name == "New");
    tab(editor, model, 4); // Delete.
    editor.handleEvent(ftxui::Event::Return, model);
    CHECK(model.entries().size() == 3);
    CHECK(model.entries()[2] == library::Credit{"B", CreditKind::Ensemble, "second"});
    CHECK(editor.handleEvent(ftxui::Event::Escape, model) == TrackCreditsEditor::Request::Cancel);
  }

  TEST_CASE("TrackCreditsEditor - failed commit localizes a later blank name and returns input to that row",
            "[tui][unit][credits]")
  {
    using library::CreditKind;

    for (auto const* const name : {"", "   "})
    {
      DYNAMIC_SECTION("Name '" << name << "'")
      {
        auto sections = emptySections();
        sections[0].optValue = std::vector<library::Credit>{{"First", CreditKind::Conductor, "guest"},
                                                            {"Second", CreditKind::Conductor, "assistant"},
                                                            {"First", CreditKind::Conductor, "guest"}};
        auto model = uimodel::TrackCreditsEditorModel{};
        REQUIRE(model.begin(sections, uimodel::trackCreditScope(CreditKind::Conductor)));
        model.updateName(1, name);
        auto const draft = model.entries();
        auto editor = TrackCreditsEditor{ao::test::messageCatalog("de")};
        editor.reset(model);
        CHECK(editor.handleEvent(ftxui::Event::CtrlS, model) == TrackCreditsEditor::Request::None);
        CHECK(model.focusedRow() == 1);
        CHECK(model.isEditing());
        CHECK(model.entries() == draft);
        CHECK_FALSE(model.buildCommitPatch());
        auto const rendered = renderElementFit(editor.render(model)).text;
        CHECK(rendered.contains("Zeile 2: Geben Sie einen Namen ein."));
        CHECK_FALSE(rendered.contains("Zeile 1:"));

        editor.handleEvent(ftxui::Event::Character("Repaired"), model);
        CHECK(model.entries()[1].name == std::string{name} + "Repaired");
        CHECK(model.entries()[1].role == "assistant");
        CHECK(editor.handleEvent(ftxui::Event::CtrlS, model) == TrackCreditsEditor::Request::Accept);
        auto const patchRes = model.buildCommitPatch();
        REQUIRE(patchRes);
        REQUIRE(patchRes->optCredits);
        CHECK(patchRes->optCredits->entries ==
              std::vector<library::Credit>{{"First", CreditKind::Conductor, "guest"},
                                           {"Repaired", CreditKind::Conductor, "assistant"},
                                           {"First", CreditKind::Conductor, "guest"}});
      }
    }
  }

  TEST_CASE("TrackCreditsEditor - failed commit localizes later text errors and focuses the offending field",
            "[tui][unit][credits]")
  {
    using library::CreditKind;
    auto sections = emptySections();
    sections[2].optValue = std::vector<library::Credit>{{"First", CreditKind::Soloist, "violin"},
                                                        {"Second", CreditKind::Soloist, "cello"},
                                                        {"First", CreditKind::Soloist, "violin"}};
    auto model = uimodel::TrackCreditsEditorModel{};
    REQUIRE(model.begin(sections, uimodel::trackCreditScope(CreditKind::Soloist)));
    bool role = false;
    auto message = std::string{"Zeile 2: Der Name enthält ungültigen Text."};

    SECTION("invalid name returns to Name")
    {
      model.updateName(1, "bad\xff");
    }

    SECTION("invalid role returns to Role without replacing the name")
    {
      role = true;
      message = "Zeile 2: Die Rolle oder das Instrument enthält ungültigen Text.";
      model.updateRole(1, "bad\xff");
    }

    auto const draft = model.entries();
    auto editor = TrackCreditsEditor{ao::test::messageCatalog("de")};
    editor.reset(model);
    CHECK(editor.handleEvent(ftxui::Event::CtrlS, model) == TrackCreditsEditor::Request::None);
    CHECK(model.focusedRow() == 1);
    CHECK(model.entries() == draft);
    CHECK_FALSE(model.buildCommitPatch());
    auto const rendered = renderElementFit(editor.render(model)).text;
    CHECK(rendered.contains(message));
    CHECK_FALSE(rendered.contains("entry 0"));
    CHECK_FALSE(rendered.contains("Zeile 1:"));

    editor.handleEvent(ftxui::Event::Character("Repaired"), model);
    CHECK(model.entries()[1].name == (role ? "Second" : "Repaired"));
    CHECK(model.entries()[1].role == (role ? "Repaired" : "cello"));
    CHECK(model.entries()[0] == draft[0]);
    CHECK(model.entries()[2] == draft[2]);
    CHECK(editor.handleEvent(ftxui::Event::CtrlS, model) == TrackCreditsEditor::Request::Accept);
    auto const patchRes = model.buildCommitPatch();
    REQUIRE(patchRes);
    REQUIRE(patchRes->optCredits);
    CHECK(patchRes->optCredits->kinds == uimodel::trackCreditScope(CreditKind::Soloist));
    CHECK(patchRes->optCredits->entries == model.entries());
  }

  TEST_CASE("TrackCreditsEditor - scope caption follows the fixed scope rather than the selected row kind",
            "[tui][unit][credits]")
  {
    auto sections = emptySections();
    sections[2].optValue = std::vector<library::Credit>{{"Solo", library::CreditKind::Soloist, "violin"}};
    auto model = uimodel::TrackCreditsEditorModel{};
    auto editor = TrackCreditsEditor{ao::test::messageCatalog("de")};

    SECTION("one-kind scope names the category")
    {
      REQUIRE(model.begin(sections, uimodel::trackCreditScope(library::CreditKind::Soloist)));
      editor.reset(model);
      CHECK(renderElementFit(editor.render(model)).text.contains("Mitwirkungsbereich: Solist"));
    }

    SECTION("all-kind scope does not become the current row category")
    {
      REQUIRE(model.begin(sections, uimodel::allTrackCreditKinds()));
      editor.reset(model);
      CHECK(renderElementFit(editor.render(model)).text.contains("Mitwirkungsbereich: Alle Mitwirkungskategorien"));
    }
  }

  TEST_CASE("TrackCreditsEditor - mixed intent cannot accidentally clear and explicit clear names its scope",
            "[tui][unit][credits]")
  {
    auto sections = emptySections();
    sections[3] = {.mixed = true};
    auto model = uimodel::TrackCreditsEditorModel{};
    REQUIRE(model.begin(sections, uimodel::trackCreditScope(library::CreditKind::Performer)));
    auto editor = TrackCreditsEditor{ao::test::englishMessageCatalog()};
    editor.reset(model);
    CHECK_FALSE(model.canEdit());
    CHECK(editor.handleEvent(ftxui::Event::CtrlS, model) == TrackCreditsEditor::Request::None);
    editor.reset(model);
    editor.handleEvent(ftxui::Event::Return, model); // Explicit replace, now Add.
    CHECK(model.canEdit());
    CHECK(editor.handleEvent(ftxui::Event::CtrlS, model) == TrackCreditsEditor::Request::None);
    editor.handleEvent(ftxui::Event::TabReverse, model); // Clear.
    editor.handleEvent(ftxui::Event::Return, model);
    CHECK(editor.handleEvent(ftxui::Event::CtrlS, model) == TrackCreditsEditor::Request::Accept);
    auto const patchRes = model.buildCommitPatch();
    REQUIRE(patchRes);
    CHECK(patchRes->optCredits->entries.empty());
    CHECK(patchRes->optCredits->kinds == uimodel::trackCreditScope(library::CreditKind::Performer));
  }

  TEST_CASE("TrackCreditsEditor - completion follows the selected kind and keeps role vocabulary separate",
            "[tui][unit][credits]")
  {
    using library::CreditKind;
    auto sections = emptySections();
    sections[2].optValue = std::vector<library::Credit>{{"A", CreditKind::Soloist, ""}};
    auto model = uimodel::TrackCreditsEditorModel{};
    REQUIRE(model.begin(sections, uimodel::allTrackCreditKinds()));
    auto kinds = std::vector<CreditKind>{};
    auto roles = std::vector<bool>{};
    auto editor = TrackCreditsEditor{
      ao::test::englishMessageCatalog(),
      [&](CreditKind const kind, bool const role, std::string_view const text, std::size_t)
      {
        kinds.push_back(kind);
        roles.push_back(role);
        return std::optional{rt::CompletionResult{
          .replaceBegin = 0,
          .replaceEnd = text.size(),
          .items = {{.displayText = role ? "Violin" : "Anne", .insertText = role ? "Violin" : "Anne"}}}};
      }};
    editor.reset(model);
    editor.handleEvent(ftxui::Event::CtrlN, model);
    editor.handleEvent(ftxui::Event::Return, model);
    tab(editor, model, 1);
    editor.handleEvent(ftxui::Event::CtrlN, model);
    editor.handleEvent(ftxui::Event::Return, model);
    tab(editor, model, 1);
    editor.handleEvent(ftxui::Event::Return, model); // Soloist to Performer.
    editor.handleEvent(ftxui::Event::TabReverse, model);
    editor.handleEvent(ftxui::Event::TabReverse, model);
    editor.handleEvent(ftxui::Event::CtrlN, model);
    CHECK(kinds == std::vector<CreditKind>{CreditKind::Soloist, CreditKind::Soloist, CreditKind::Performer});
    CHECK(roles == std::vector<bool>{false, true, false});
    CHECK(model.entries() == std::vector<library::Credit>{{"Anne", CreditKind::Performer, "Violin"}});
    CHECK(editor.handleEvent(ftxui::Event::Escape, model) == TrackCreditsEditor::Request::None);
    CHECK(editor.handleEvent(ftxui::Event::Escape, model) == TrackCreditsEditor::Request::Cancel);
  }

  TEST_CASE("TrackCreditsEditor - completion paging preserves drafts and restores the scrolled input caret",
            "[tui][unit][credits]")
  {
    for (auto const columns : {32, 80})
    {
      for (bool const role : {false, true})
      {
        DYNAMIC_SECTION((role ? "Role" : "Name") << " at " << columns << " columns")
        {
          auto sections = emptySections();
          auto const original = library::Credit{
            std::string(90, 'x') + "NameEnd", library::CreditKind::Soloist, std::string(90, 'y') + "RoleEnd"};
          sections[2].optValue = std::vector{original};
          auto model = uimodel::TrackCreditsEditorModel{};
          REQUIRE(model.begin(sections, uimodel::trackCreditScope(library::CreditKind::Soloist)));
          auto editor =
            TrackCreditsEditor{ao::test::englishMessageCatalog(),
                               [&](library::CreditKind const kind,
                                   bool const completingRole,
                                   std::string_view const text,
                                   std::size_t const cursor)
                               {
                                 CHECK(kind == library::CreditKind::Soloist);
                                 CHECK(completingRole == role);
                                 CHECK(text == (role ? original.role : original.name));
                                 CHECK(cursor == text.size());
                                 auto items = std::vector<rt::CompletionItem>{};

                                 for (std::size_t index = 0; index < 15; ++index)
                                 {
                                   auto candidate = std::format("Cand{:02}", index);
                                   items.push_back({.displayText = candidate, .insertText = candidate});
                                 }

                                 return std::optional{rt::CompletionResult{
                                   .replaceBegin = 0, .replaceEnd = text.size(), .items = std::move(items)}};
                               }};
          editor.reset(model);
          tab(editor, model, role ? 1 : 0);
          editor.handleEvent(ftxui::Event::CtrlN, model);

          for (std::size_t step = 0; step < 6; ++step)
          {
            editor.handleEvent(ftxui::Event::ArrowDown, model);
          }

          editor.handleEvent(ftxui::Event::ArrowUp, model);
          auto rendered = renderElement(editor.render(model), columns, 14);
          INFO(rendered.text);
          CHECK(rendered.text.contains("> Cand05"));
          CHECK(rendered.text.contains("Cand01"));
          CHECK(rendered.text.contains("Cand06"));
          CHECK_FALSE(rendered.text.contains("Cand00"));
          editor.handleEvent(ftxui::Event::PageDown, model);
          rendered = renderElement(editor.render(model), columns, 14);
          CHECK(rendered.text.contains("> Cand11"));
          CHECK_FALSE(rendered.text.contains("Cand06"));
          editor.handleEvent(ftxui::Event::PageUp, model);
          CHECK(renderElement(editor.render(model), columns, 14).text.contains("> Cand05"));
          CHECK(model.entries() == std::vector<library::Credit>{original});
          auto const patchRes = model.buildCommitPatch();
          REQUIRE(patchRes);
          CHECK_FALSE(patchRes->optCredits);

          CHECK(editor.handleEvent(ftxui::Event::Escape, model) == TrackCreditsEditor::Request::None);
          rendered = renderElement(editor.render(model), columns, 14);
          CHECK_FALSE(rendered.text.contains("Cand05"));
          auto const optEnd = findTextCells(rendered.screen, role ? "RoleEnd" : "NameEnd");
          REQUIRE(optEnd);
          REQUIRE(optEnd->x_max + 1 < rendered.screen.dimx());
          CHECK(rendered.screen.PixelAt(optEnd->x_max + 1, optEnd->y_min).inverted);
          CHECK(model.entries() == std::vector<library::Credit>{original});
          auto const dismissedPatchRes = model.buildCommitPatch();
          REQUIRE(dismissedPatchRes);
          CHECK_FALSE(dismissedPatchRes->optCredits);
          CHECK(editor.handleEvent(ftxui::Event::Escape, model) == TrackCreditsEditor::Request::Cancel);
        }
      }
    }
  }

  TEST_CASE("TrackCreditsEditor - controls remain keyboard reachable in constrained localized layouts",
            "[tui][unit][credits]")
  {
    auto sections = emptySections();
    auto model = uimodel::TrackCreditsEditorModel{};
    REQUIRE(model.begin(sections, uimodel::allTrackCreditKinds()));

    for (auto const* const locale : {"en", "de", "zh-Hans"})
    {
      auto editor = TrackCreditsEditor{ao::test::messageCatalog(locale)};

      for (auto const columns : {32, 80})
      {
        editor.reset(model);
        tab(editor, model, 3);
        auto const addFrame = renderElement(editor.render(model), columns, 14).text;
        CHECK(addFrame.contains("Enter"));
        editor.handleEvent(ftxui::Event::Return, model);
        CHECK_FALSE(model.entries().empty());
        editor.handleEvent(ftxui::Event::Character("Name"), model);
        CHECK(editor.handleEvent(ftxui::Event::CtrlS, model) == TrackCreditsEditor::Request::Accept);
        auto const commitFrame = renderElement(editor.render(model), columns, 14).text;
        CHECK(commitFrame.contains("Ctrl-S"));
        model.clearScope();
      }
    }
  }
} // namespace ao::tui::test
