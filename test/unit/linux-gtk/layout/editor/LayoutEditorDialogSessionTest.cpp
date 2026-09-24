// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "app/ShellLayoutCollaborators.h"
#include "app/linux-gtk/layout/editor/LayoutEditorDialog.h"
#include "app/linux-gtk/layout/runtime/ActionRegistry.h"
#include "app/linux-gtk/layout/runtime/ComponentRegistry.h"
#include "app/linux-gtk/layout/runtime/LayoutRuntime.h"
#include "layout/document/LayoutPresets.h"
#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"
#include "test/unit/linux-gtk/GtkRuntimeTestSupport.h"
#include "test/unit/linux-gtk/GtkWidgetTestSupport.h"
#include "test/unit/linux-gtk/layout/LayoutTestSupport.h"
#include <ao/Error.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/uimodel/layout/document/LayoutDocument.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/playback/output/OutputDeviceIntent.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/application.h>
#include <gtkmm/button.h>
#include <gtkmm/comboboxtext.h>
#include <gtkmm/dialog.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/treeview.h>
#include <gtkmm/widget.h>
#include <gtkmm/window.h>
#include <sigc++/signal.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::gtk::layout::editor::test
{
  using namespace uimodel;
  using ao::gtk::layout::test::encodedLayout;
  using ao::gtk::test::collectAll;
  using ao::gtk::test::emitClicked;
  using ao::gtk::test::findWidget;

  namespace
  {
    struct DialogSessionFixture final
    {
      DialogSessionFixture()
      {
        LayoutRuntime::registerStandardComponents(
          registry,
          *runtimePtr,
          ShellLayoutCollaborators{
            .textCatalog = textCatalog, .outputDeviceIntent = uimodel::OutputDeviceIntent::discarded()});
        actionRegistry.tryRegisterAction(
          {.id = "playback.showOutputDeviceSelector", .label = "Output Device", .category = "Playback"}, {});
        actionRegistry.tryRegisterAction(
          {.id = "shell.showSystemMenu", .label = "System Menu", .category = "Shell"}, {});
        actionRegistry.tryRegisterAction({.id = "shell.showSoul", .label = "Show Soul", .category = "Shell"}, {});
      }

      Glib::RefPtr<Gtk::Application> appPtr = Gtk::Application::create("io.github.aobus.layout_editor_session_test");
      ao::test::TempDir tempDir{};
      std::unique_ptr<rt::AppRuntime> runtimePtr = ao::gtk::test::makeRuntime(tempDir);
      i18n::MessageCatalog textCatalog = ao::test::englishMessageCatalog();
      ComponentRegistry registry{};
      ActionRegistry actionRegistry{registry.schema()};
      Gtk::Window window{};
      LayoutDocument doc = makeDefaultLayout();
    };

    Gtk::ComboBoxText* presetCombo(LayoutEditorDialog& dialog)
    {
      auto combos = collectAll<Gtk::ComboBoxText>(dialog);
      REQUIRE(combos.size() == 2);

      auto* const combo = combos[0]->get_active_id() == "classic" ? combos[0] : combos[1];
      REQUIRE(combo != nullptr);
      return combo;
    }

    Gtk::Button* resetDefaultButton(LayoutEditorDialog& dialog)
    {
      for (auto* const button : collectAll<Gtk::Button>(dialog.headerBar()))
      {
        if (button->get_tooltip_text() == "Reset selected preset to its default layout")
        {
          return button;
        }
      }

      return nullptr;
    }

    void selectRoot(LayoutEditorDialog& dialog)
    {
      auto* const treeView = findWidget<Gtk::TreeView>(dialog);
      REQUIRE(treeView != nullptr);

      if (auto const modelPtr = treeView->get_model(); modelPtr && !modelPtr->children().empty())
      {
        treeView->get_selection()->select(modelPtr->children().begin());
      }
    }

    LayoutDocument presetRootDocument(std::string_view presetId)
    {
      auto testDoc = LayoutDocument{};
      testDoc.root.type = "box";
      testDoc.root.id = std::string{presetId} + "_root";
      return testDoc;
    }

    LayoutDocument emptyLayout(std::string_view /*unused*/)
    {
      return {};
    }
  } // namespace

  TEST_CASE("LayoutEditorDialog - preset session cache preserves dirty edits", "[gtk][unit][layout-editor][session]")
  {
    auto fixture = DialogSessionFixture{};
    std::int32_t loadCount = 0;
    auto loadedPresets = std::vector<std::string>{};
    auto const customLoader = [&](std::string_view presetId)
    {
      ++loadCount;
      loadedPresets.emplace_back(presetId);
      return presetRootDocument(presetId);
    };

    auto dialog = LayoutEditorDialog{fixture.window,
                                     fixture.registry,
                                     fixture.actionRegistry,
                                     ao::test::englishMessageCatalog(),
                                     fixture.doc,
                                     "classic",
                                     "modern",
                                     customLoader};

    auto* const combo = presetCombo(dialog);
    CHECK(combo->get_active_id() == "classic");

    selectRoot(dialog);
    auto const initialCount = dialog.document().root.children.size();

    CHECK(dialog.activate_action("editor.add_spacer"));
    CHECK(dialog.document().root.children.size() == initialCount + 1);

    combo->set_active_id("modern");

    CHECK(loadCount == 1);
    CHECK(loadedPresets.back() == "modern");
    CHECK(dialog.document().root.id == "modern_root");

    combo->set_active_id("classic");
    CHECK(loadCount == 1);
    CHECK(dialog.document().root.children.size() == initialCount + 1);
    auto const optExpectedActiveDocument = encodedLayout(dialog.document());
    REQUIRE(optExpectedActiveDocument);

    auto saveResult = LayoutSaveResult{};
    std::int32_t saveCount = 0;
    dialog.signalSaveRequest().connect(
      [&](LayoutSaveResult const& res)
      {
        saveResult = res;
        ++saveCount;
        return Result<>{};
      });

    dialog.response(Gtk::ResponseType::OK);
    CHECK(saveCount == 1);
    CHECK(saveResult.activePresetId == "classic");
    REQUIRE(saveResult.modified.size() == 1);
    REQUIRE(saveResult.modified.contains("classic"));
    CHECK(saveResult.resets.empty());
    auto const optActiveDocument = encodedLayout(saveResult.activeDocument);
    auto const optModifiedClassic = encodedLayout(saveResult.modified.at("classic"));
    REQUIRE(optActiveDocument);
    REQUIRE(optModifiedClassic);
    CHECK(*optActiveDocument == *optExpectedActiveDocument);
    CHECK(*optModifiedClassic == *optExpectedActiveDocument);

    dialog.close();
  }

  TEST_CASE("LayoutEditorDialog - reset default records reset and modified preset",
            "[gtk][unit][layout-editor][session]")
  {
    auto fixture = DialogSessionFixture{};
    auto const customLoader = [](std::string_view presetId) { return presetRootDocument(presetId); };
    auto dialog = LayoutEditorDialog{fixture.window,
                                     fixture.registry,
                                     fixture.actionRegistry,
                                     ao::test::englishMessageCatalog(),
                                     fixture.doc,
                                     "classic",
                                     "modern",
                                     customLoader};

    auto* const combo = presetCombo(dialog);
    auto* const resetButton = resetDefaultButton(dialog);
    REQUIRE(resetButton != nullptr);
    emitClicked(*resetButton);

    combo->set_active_id("modern");
    selectRoot(dialog);
    CHECK(dialog.activate_action("editor.add_spacer"));
    auto const optExpectedActiveDocument = encodedLayout(dialog.document());
    REQUIRE(optExpectedActiveDocument);

    auto saveResult = LayoutSaveResult{};
    std::int32_t saveCount = 0;
    dialog.signalSaveRequest().connect(
      [&](LayoutSaveResult const& res)
      {
        saveResult = res;
        ++saveCount;
        return Result<>{};
      });

    dialog.response(Gtk::ResponseType::OK);
    CHECK(saveCount == 1);
    CHECK(saveResult.activePresetId == "modern");
    auto const expectedResets = std::vector<std::string>{"classic"};
    CHECK(saveResult.resets == expectedResets);
    REQUIRE(saveResult.modified.size() == 1);
    REQUIRE(saveResult.modified.contains("modern"));
    auto const optActiveDocument = encodedLayout(saveResult.activeDocument);
    auto const optModifiedModern = encodedLayout(saveResult.modified.at("modern"));
    REQUIRE(optActiveDocument);
    REQUIRE(optModifiedModern);
    CHECK(*optActiveDocument == *optExpectedActiveDocument);
    CHECK(*optModifiedModern == *optExpectedActiveDocument);

    dialog.close();
  }

  TEST_CASE("LayoutEditorDialog - dirty invalid background preset blocks save", "[gtk][unit][layout-editor][session]")
  {
    auto fixture = DialogSessionFixture{};
    std::int32_t loadCount = 0;
    auto loadedPresets = std::vector<std::string>{};
    auto const customLoader = [&](std::string_view presetId)
    {
      ++loadCount;
      loadedPresets.emplace_back(presetId);

      if (presetId == "modern")
      {
        auto testDoc = presetRootDocument(presetId);
        auto invalidChild = LayoutNode{.type = "actionButton"};
        invalidChild.props["primaryAction"] = LayoutValue{"this.does.not.exist"};
        testDoc.root.children.push_back(std::move(invalidChild));
        return testDoc;
      }

      return presetRootDocument(presetId);
    };

    auto dialog = LayoutEditorDialog{fixture.window,
                                     fixture.registry,
                                     fixture.actionRegistry,
                                     ao::test::englishMessageCatalog(),
                                     fixture.doc,
                                     "classic",
                                     "modern",
                                     customLoader};
    auto* const combo = presetCombo(dialog);

    combo->set_active_id("modern");
    CHECK(loadCount == 1);
    combo->set_active_id("classic");
    CHECK(loadCount == 1);
    combo->set_active_id("modern");
    CHECK(loadCount == 1);

    selectRoot(dialog);
    REQUIRE(dialog.activate_action("editor.add_spacer"));
    REQUIRE(dialog.document().root.children.size() == 2);
    combo->set_active_id("classic");

    auto saveResult = LayoutSaveResult{};
    std::int32_t saveCount = 0;
    dialog.signalSaveRequest().connect(
      [&](LayoutSaveResult const& res)
      {
        saveResult = res;
        ++saveCount;
        return Result<>{};
      });

    dialog.response(Gtk::ResponseType::OK);
    CHECK(saveCount == 0);

    dialog.close();
  }

  TEST_CASE("LayoutEditorDialog - active preset reset is saved without switching",
            "[gtk][unit][layout-editor][session]")
  {
    auto fixture = DialogSessionFixture{};
    auto dialog = LayoutEditorDialog{fixture.window,
                                     fixture.registry,
                                     fixture.actionRegistry,
                                     ao::test::englishMessageCatalog(),
                                     fixture.doc,
                                     "classic",
                                     "modern",
                                     emptyLayout};

    auto* const resetButton = resetDefaultButton(dialog);
    REQUIRE(resetButton != nullptr);
    emitClicked(*resetButton);
    auto const optExpectedActiveDocument = encodedLayout(makeDefaultLayout());
    REQUIRE(optExpectedActiveDocument);

    auto saveResult = LayoutSaveResult{};
    std::int32_t saveCount = 0;
    dialog.signalSaveRequest().connect(
      [&](LayoutSaveResult const& res)
      {
        saveResult = res;
        ++saveCount;
        return Result<>{};
      });

    dialog.response(Gtk::ResponseType::OK);
    CHECK(saveCount == 1);
    CHECK(saveResult.activePresetId == "classic");
    CHECK(saveResult.modified.empty());
    auto const expectedResets = std::vector<std::string>{"classic"};
    CHECK(saveResult.resets == expectedResets);
    auto const optActiveDocument = encodedLayout(saveResult.activeDocument);
    REQUIRE(optActiveDocument);
    CHECK(*optActiveDocument == *optExpectedActiveDocument);

    dialog.close();
  }

  TEST_CASE("LayoutEditorDialog - reset with no active preset leaves the draft unchanged",
            "[gtk][unit][layout-editor][session]")
  {
    auto fixture = DialogSessionFixture{};
    auto dialog = LayoutEditorDialog{fixture.window,
                                     fixture.registry,
                                     fixture.actionRegistry,
                                     ao::test::englishMessageCatalog(),
                                     fixture.doc,
                                     "classic",
                                     "modern",
                                     emptyLayout};

    auto const optBeforeReset = encodedLayout(dialog.document());
    REQUIRE(optBeforeReset);

    auto* const combo = presetCombo(dialog);
    combo->set_active(-1);

    auto* const resetButton = resetDefaultButton(dialog);
    REQUIRE(resetButton != nullptr);
    emitClicked(*resetButton);

    auto const optAfterReset = encodedLayout(dialog.document());
    REQUIRE(optAfterReset);
    CHECK(*optAfterReset == *optBeforeReset);

    // A missing active row is not a save policy. Restore the valid session
    // identity before observing the reset no-op in a save result.
    combo->set_active_id("classic");
    REQUIRE(combo->get_active_id() == "classic");

    auto saveResult = LayoutSaveResult{};
    std::int32_t saveCount = 0;
    dialog.signalSaveRequest().connect(
      [&](LayoutSaveResult const& res)
      {
        saveResult = res;
        ++saveCount;
        return Result<>{};
      });

    dialog.response(Gtk::ResponseType::OK);
    CHECK(saveCount == 1);
    CHECK(saveResult.activePresetId == "classic");
    CHECK(saveResult.resets.empty());
    CHECK(saveResult.modified.empty());
    auto const optActiveDocument = encodedLayout(saveResult.activeDocument);
    REQUIRE(optActiveDocument);
    CHECK(*optActiveDocument == *optBeforeReset);

    dialog.close();
  }

  TEST_CASE("LayoutEditorDialog - terminal responses retire a pending property preview",
            "[gtk][unit][layout-editor][session][async]")
  {
    auto fixture = DialogSessionFixture{};
    auto manualScheduler = sigc::signal<bool()>{};
    std::int32_t scheduledPreviewCount = 0;
    auto const scheduler = [&](std::function<bool()> callback)
    {
      ++scheduledPreviewCount;
      return manualScheduler.connect(std::move(callback));
    };
    auto dialog = LayoutEditorDialog{fixture.window,
                                     fixture.registry,
                                     fixture.actionRegistry,
                                     ao::test::englishMessageCatalog(),
                                     fixture.doc,
                                     "classic",
                                     "modern",
                                     presetRootDocument,
                                     scheduler};
    std::int32_t previewCount = 0;
    std::int32_t saveCount = 0;
    dialog.signalApplyPreview().connect([&](LayoutDocument const&) { ++previewCount; });
    dialog.signalSaveRequest().connect(
      [&](LayoutSaveResult const&) -> Result<>
      {
        ++saveCount;
        return {};
      });

    selectRoot(dialog);
    auto const spinButtons = collectAll<Gtk::SpinButton>(dialog);
    REQUIRE(!spinButtons.empty());
    spinButtons.front()->set_value(spinButtons.front()->get_value() + 1.0);
    REQUIRE(scheduledPreviewCount == 1);
    REQUIRE(previewCount == 0);
    dialog.present();
    REQUIRE(ao::gtk::test::tryPumpGtkEventsUntil([&] { return dialog.get_mapped(); }));

    SECTION("Cancel response")
    {
      dialog.response(Gtk::ResponseType::CANCEL);
      CHECK(saveCount == 0);
    }

    SECTION("ordinary close")
    {
      dialog.close();
      CHECK(saveCount == 0);
    }

    SECTION("successful Save response")
    {
      dialog.response(Gtk::ResponseType::OK);
      REQUIRE(saveCount == 1);
    }

    REQUIRE_FALSE(dialog.get_visible());
    manualScheduler.emit();
    CHECK(previewCount == 0);
  }

  TEST_CASE("LayoutEditorDialog - preset rebind retires the pending property preview",
            "[gtk][unit][layout-editor][session][async]")
  {
    auto fixture = DialogSessionFixture{};
    auto manualScheduler = sigc::signal<bool()>{};
    std::int32_t scheduledPreviewCount = 0;
    auto const scheduler = [&](std::function<bool()> callback)
    {
      ++scheduledPreviewCount;
      return manualScheduler.connect(std::move(callback));
    };
    auto dialog = LayoutEditorDialog{fixture.window,
                                     fixture.registry,
                                     fixture.actionRegistry,
                                     ao::test::englishMessageCatalog(),
                                     fixture.doc,
                                     "classic",
                                     "modern",
                                     presetRootDocument,
                                     scheduler};
    auto previews = std::vector<std::pair<std::string, std::size_t>>{};
    dialog.signalApplyPreview().connect([&](LayoutDocument const& document)
                                        { previews.emplace_back(document.root.id, document.root.children.size()); });

    auto* const combo = presetCombo(dialog);
    selectRoot(dialog);
    auto const spinButtons = collectAll<Gtk::SpinButton>(dialog);
    REQUIRE(!spinButtons.empty());
    spinButtons.front()->set_value(spinButtons.front()->get_value() + 1.0);
    REQUIRE(scheduledPreviewCount == 1);
    CHECK(previews.empty());

    combo->set_active_id("modern");
    auto const expected = std::vector<std::pair<std::string, std::size_t>>{{"modern_root", 0}};
    CHECK(previews == expected);

    manualScheduler.emit();
    CHECK(previews == expected);

    dialog.close();
  }
} // namespace ao::gtk::layout::editor::test
