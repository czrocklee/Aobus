// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "app/linux-gtk/layout/editor/LayoutEditorDialog.h"
#include "app/linux-gtk/layout/runtime/ActionRegistry.h"
#include "app/linux-gtk/layout/runtime/ComponentRegistry.h"
#include "app/linux-gtk/layout/runtime/LayoutRuntime.h"
#include "layout/document/LayoutDocument.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/uimodel/layout/document/LayoutDocument.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/layout/document/LayoutYaml.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/application.h>
#include <gtkmm/dialog.h>
#include <gtkmm/treeview.h>
#include <gtkmm/widget.h>
#include <gtkmm/window.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ao::gtk::layout::editor::test
{
  using namespace uimodel;
  using ao::gtk::test::findWidget;

  // ---------------------------------------------------------------------------
  // LayoutEditorDialog
  // ---------------------------------------------------------------------------
  TEST_CASE("LayoutEditorDialog - renders and edits the current layout document", "[gtk][unit][layout][editor]")
  {
    auto const appPtr = Gtk::Application::create("io.github.aobus.layout_editor_test");

    auto registry = ComponentRegistry{};
    LayoutRuntime::registerStandardComponents(registry);
    auto actionRegistry = ActionRegistry{};

    auto window = Gtk::Window{};
    auto const doc = makeDefaultLayout();
    auto const stubLoader = [](std::string_view) { return uimodel::LayoutDocument{}; };

    SECTION("Dialog initializes selectors and tree")
    {
      auto dialogPtr =
        std::make_unique<LayoutEditorDialog>(window, registry, actionRegistry, doc, "classic", "modern", stubLoader);

      CHECK(dialogPtr->selectedPresetId() == "classic");
      CHECK(dialogPtr->selectedThemeId() == "modern");

      auto* const treeView = findWidget<Gtk::TreeView>(*dialogPtr);
      REQUIRE(treeView != nullptr);
      auto const modelPtr = treeView->get_model();
      REQUIRE(modelPtr);
      REQUIRE(!modelPtr->children().empty());
      dialogPtr->close();
    }

    SECTION("document returns the initial document on construction")
    {
      auto dialogPtr =
        std::make_unique<LayoutEditorDialog>(window, registry, actionRegistry, doc, "classic", "modern", stubLoader);
      auto const& returned = dialogPtr->document();

      CHECK(returned.root.type == doc.root.type);
      CHECK(returned.root.id == doc.root.id);

      dialogPtr->close();
    }

    SECTION("invalid save does not emit save request")
    {
      auto invalidDoc = LayoutDocument{};
      invalidDoc.root.type = "app.actionButton";
      invalidDoc.root.props["primaryAction"] = LayoutValue{std::string{"this.does.not.exist"}};

      auto dialog = LayoutEditorDialog{window, registry, actionRegistry, invalidDoc, "classic", "modern", stubLoader};

      std::int32_t saveCount = 0;
      dialog.signalSaveRequest().connect([&](LayoutSaveResult const&) { ++saveCount; });

      dialog.response(Gtk::ResponseType::OK);

      CHECK(saveCount == 0);
      dialog.close();
    }

    SECTION("signalApplyPreview is emitted on document changes")
    {
      auto dialogPtr =
        std::make_unique<LayoutEditorDialog>(window, registry, actionRegistry, doc, "classic", "modern", stubLoader);
      std::int32_t count = 0;

      dialogPtr->signalApplyPreview().connect([&](LayoutDocument const&) { ++count; });

      auto* const treeView = findWidget<Gtk::TreeView>(*dialogPtr);
      REQUIRE(treeView != nullptr);

      if (auto const modelPtr = treeView->get_model(); modelPtr && !modelPtr->children().empty())
      {
        treeView->get_selection()->select(modelPtr->children().begin());
      }

      CHECK(dialogPtr->activate_action("editor.add_spacer"));

      CHECK(count > 0);

      dialogPtr->close();
    }

    SECTION("added components receive unique ids")
    {
      auto dialog = LayoutEditorDialog{window, registry, actionRegistry, doc, "classic", "modern", stubLoader};

      auto* const treeView = findWidget<Gtk::TreeView>(dialog);
      REQUIRE(treeView != nullptr);

      auto const selectRoot = [treeView]
      {
        if (auto const modelPtr = treeView->get_model(); modelPtr && !modelPtr->children().empty())
        {
          treeView->get_selection()->select(modelPtr->children().begin());
        }
      };

      auto const initialCount = dialog.document().root.children.size();

      selectRoot();
      CHECK(dialog.activate_action("editor.add_spacer"));
      selectRoot();
      CHECK(dialog.activate_action("editor.add_spacer"));

      REQUIRE(dialog.document().root.children.size() == initialCount + 2);
      auto const& firstAdded = dialog.document().root.children[initialCount];
      auto const& secondAdded = dialog.document().root.children[initialCount + 1];
      CHECK(firstAdded.id == "spacer-new");
      CHECK(secondAdded.id == "spacer-new-2");

      dialog.close();
    }

    SECTION("duplicate stateful ids prevent save")
    {
      auto duplicateDoc = LayoutDocument{};
      duplicateDoc.root.type = "box";
      duplicateDoc.root.children = {
        LayoutNode{.id = "shared-split", .type = "split"},
        LayoutNode{.id = "shared-split", .type = "collapsibleSplit"},
      };

      auto dialog = LayoutEditorDialog{window, registry, actionRegistry, duplicateDoc, "classic", "modern", stubLoader};

      std::int32_t saveCount = 0;
      dialog.signalSaveRequest().connect([&](LayoutSaveResult const&) { ++saveCount; });

      dialog.response(Gtk::ResponseType::OK);

      CHECK(saveCount == 0);
      dialog.close();
    }

    SECTION("wrapped nodes receive a unique container id")
    {
      auto dialog = LayoutEditorDialog{window, registry, actionRegistry, doc, "classic", "modern", stubLoader};

      auto* const treeView = findWidget<Gtk::TreeView>(dialog);
      REQUIRE(treeView != nullptr);
      auto const modelPtr = treeView->get_model();
      REQUIRE(modelPtr);
      REQUIRE(!modelPtr->children().empty());

      auto const rootRow = *modelPtr->children().begin();
      REQUIRE(!rootRow.children().empty());
      treeView->get_selection()->select(rootRow.children().begin());

      auto const originalFirstChildId = dialog.document().root.children.front().id;
      CHECK(dialog.activate_action("editor.wrap_box"));

      auto const& wrapper = dialog.document().root.children.front();
      CHECK(wrapper.id == "box-wrap");
      REQUIRE(wrapper.children.size() == 1);
      CHECK(wrapper.children.front().id == originalFirstChildId);

      dialog.close();
    }
  }
} // namespace ao::gtk::layout::editor::test
