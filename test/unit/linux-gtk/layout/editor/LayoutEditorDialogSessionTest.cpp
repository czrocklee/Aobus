// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "../../GtkTestSupport.h"
#include "app/linux-gtk/layout/editor/LayoutEditorDialog.h"
#include "app/linux-gtk/layout/runtime/ActionRegistry.h"
#include "app/linux-gtk/layout/runtime/ComponentRegistry.h"
#include "app/linux-gtk/layout/runtime/LayoutRuntime.h"
#include "layout/document/LayoutDocument.h"
#include <ao/uimodel/layout/document/LayoutDocument.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/application.h>
#include <gtkmm/button.h>
#include <gtkmm/comboboxtext.h>
#include <gtkmm/dialog.h>
#include <gtkmm/treeview.h>
#include <gtkmm/widget.h>
#include <gtkmm/window.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ao::gtk::layout::editor::test
{
  using namespace uimodel;
  using ao::gtk::test::collectAll;
  using ao::gtk::test::emitClicked;

  namespace
  {
    struct DialogSessionFixture final
    {
      DialogSessionFixture() { LayoutRuntime::registerStandardComponents(registry); }

      Glib::RefPtr<Gtk::Application> appPtr = Gtk::Application::create("io.github.aobus.layout_editor_session_test");
      ComponentRegistry registry{};
      ActionRegistry actionRegistry{};
      Gtk::Window window{};
      LayoutDocument doc = createDefaultLayout();
    };

    Gtk::TreeView* findTreeView(Gtk::Widget& widget)
    {
      if (auto* const treeView = dynamic_cast<Gtk::TreeView*>(&widget); treeView != nullptr)
      {
        return treeView;
      }

      for (auto* child = widget.get_first_child(); child != nullptr; child = child->get_next_sibling())
      {
        if (auto* const found = findTreeView(*child); found != nullptr)
        {
          return found;
        }
      }

      return nullptr;
    }

    void collectComboBoxes(Gtk::Widget& widget, std::vector<Gtk::ComboBoxText*>& combos)
    {
      if (auto* const combo = dynamic_cast<Gtk::ComboBoxText*>(&widget); combo != nullptr)
      {
        combos.push_back(combo);
      }

      for (auto* child = widget.get_first_child(); child != nullptr; child = child->get_next_sibling())
      {
        collectComboBoxes(*child, combos);
      }
    }

    Gtk::ComboBoxText* presetCombo(LayoutEditorDialog& dialog)
    {
      auto combos = std::vector<Gtk::ComboBoxText*>{};
      collectComboBoxes(dialog, combos);
      REQUIRE(combos.size() == 2);

      auto* const combo = combos[0]->get_active_id() == "classic" ? combos[0] : combos[1];
      REQUIRE(combo != nullptr);
      return combo;
    }

    Gtk::Button* resetDefaultButton(LayoutEditorDialog& dialog)
    {
      for (auto* const button : collectAll<Gtk::Button>(dialog.headerBar()))
      {
        if (button->get_tooltip_text() == "Reset to selected preset's default layout")
        {
          return button;
        }
      }

      return nullptr;
    }

    void selectRoot(LayoutEditorDialog& dialog)
    {
      auto* const treeView = findTreeView(dialog);
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

  TEST_CASE("LayoutEditorDialog - preset session cache preserves dirty edits", "[gtk][unit][layout][editor][session]")
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

    auto dialog = LayoutEditorDialog{
      fixture.window, fixture.registry, fixture.actionRegistry, fixture.doc, "classic", "modern", customLoader};

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

    auto saveResult = LayoutSaveResult{};
    std::int32_t saveCount = 0;
    dialog.signalSaveRequest().connect(
      [&](LayoutSaveResult const& res)
      {
        saveResult = res;
        ++saveCount;
      });

    dialog.response(Gtk::ResponseType::OK);
    CHECK(saveCount == 1);
    CHECK(saveResult.activePresetId == "classic");
    CHECK(saveResult.modified.contains("classic"));
    CHECK(!saveResult.modified.contains("modern"));
    CHECK(saveResult.resets.empty());

    dialog.close();
  }

  TEST_CASE("LayoutEditorDialog - reset default records reset and modified preset",
            "[gtk][unit][layout][editor][session]")
  {
    auto fixture = DialogSessionFixture{};
    auto const customLoader = [](std::string_view presetId) { return presetRootDocument(presetId); };
    auto dialog = LayoutEditorDialog{
      fixture.window, fixture.registry, fixture.actionRegistry, fixture.doc, "classic", "modern", customLoader};

    auto* const combo = presetCombo(dialog);
    auto* const resetButton = resetDefaultButton(dialog);
    REQUIRE(resetButton != nullptr);
    emitClicked(*resetButton);

    combo->set_active_id("modern");
    selectRoot(dialog);
    CHECK(dialog.activate_action("editor.add_spacer"));

    auto saveResult = LayoutSaveResult{};
    std::int32_t saveCount = 0;
    dialog.signalSaveRequest().connect(
      [&](LayoutSaveResult const& res)
      {
        saveResult = res;
        ++saveCount;
      });

    dialog.response(Gtk::ResponseType::OK);
    CHECK(saveCount == 1);
    CHECK(std::ranges::contains(saveResult.resets, std::string{"classic"}));
    CHECK(saveResult.modified.contains("modern"));

    dialog.close();
  }

  TEST_CASE("LayoutEditorDialog - dirty invalid background preset blocks save", "[gtk][unit][layout][editor][session]")
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
        auto testDoc = LayoutDocument{};
        testDoc.root.type = "app.actionButton";
        testDoc.root.props["primaryAction"] = LayoutValue{"this.does.not.exist"};
        return testDoc;
      }

      return presetRootDocument(presetId);
    };

    auto dialog = LayoutEditorDialog{
      fixture.window, fixture.registry, fixture.actionRegistry, fixture.doc, "classic", "modern", customLoader};
    auto* const combo = presetCombo(dialog);

    combo->set_active_id("modern");
    CHECK(loadCount == 1);
    combo->set_active_id("classic");
    CHECK(loadCount == 1);
    combo->set_active_id("modern");
    CHECK(loadCount == 1);

    dialog.updateNodePosition("", 1, 2);
    combo->set_active_id("classic");

    auto saveResult = LayoutSaveResult{};
    std::int32_t saveCount = 0;
    dialog.signalSaveRequest().connect(
      [&](LayoutSaveResult const& res)
      {
        saveResult = res;
        ++saveCount;
      });

    dialog.response(Gtk::ResponseType::OK);
    CHECK(saveCount == 0);

    dialog.close();
  }

  TEST_CASE("LayoutEditorDialog - active preset reset is saved without switching",
            "[gtk][unit][layout][editor][session]")
  {
    auto fixture = DialogSessionFixture{};
    auto dialog = LayoutEditorDialog{
      fixture.window, fixture.registry, fixture.actionRegistry, fixture.doc, "classic", "modern", emptyLayout};

    auto* const resetButton = resetDefaultButton(dialog);
    REQUIRE(resetButton != nullptr);
    emitClicked(*resetButton);

    auto saveResult = LayoutSaveResult{};
    std::int32_t saveCount = 0;
    dialog.signalSaveRequest().connect(
      [&](LayoutSaveResult const& res)
      {
        saveResult = res;
        ++saveCount;
      });

    dialog.response(Gtk::ResponseType::OK);
    CHECK(saveCount == 1);
    CHECK(std::ranges::contains(saveResult.resets, std::string{"classic"}));

    dialog.close();
  }

  TEST_CASE("LayoutEditorDialog - reset default ignores missing active preset", "[gtk][unit][layout][editor][session]")
  {
    auto fixture = DialogSessionFixture{};
    auto dialog = LayoutEditorDialog{
      fixture.window, fixture.registry, fixture.actionRegistry, fixture.doc, "classic", "modern", emptyLayout};

    auto* const combo = presetCombo(dialog);
    combo->set_active(-1);

    auto* const resetButton = resetDefaultButton(dialog);
    REQUIRE(resetButton != nullptr);
    emitClicked(*resetButton);

    auto saveResult = LayoutSaveResult{};
    std::int32_t saveCount = 0;
    dialog.signalSaveRequest().connect(
      [&](LayoutSaveResult const& res)
      {
        saveResult = res;
        ++saveCount;
      });

    dialog.response(Gtk::ResponseType::OK);
    CHECK(saveCount == 1);
    CHECK(saveResult.resets.empty());
    CHECK(saveResult.modified.empty());

    dialog.close();
  }
} // namespace ao::gtk::layout::editor::test
