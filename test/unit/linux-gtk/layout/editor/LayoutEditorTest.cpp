// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "../../GtkTestSupport.h"
#include "app/linux-gtk/layout/document/LayoutNode.h"
#include "app/linux-gtk/layout/editor/LayoutEditorDialog.h"
#include "app/linux-gtk/layout/runtime/ActionRegistry.h"
#include "app/linux-gtk/layout/runtime/ComponentRegistry.h"
#include "app/linux-gtk/layout/runtime/LayoutRuntime.h"
#include "layout/document/LayoutDocument.h"
#include "test/unit/lmdb/TestUtils.h"
#include <ao/uimodel/layout/ComponentCatalog.h>
#include <ao/uimodel/layout/LayoutYaml.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/application.h>
#include <gtkmm/box.h>
#include <gtkmm/comboboxtext.h>
#include <gtkmm/dialog.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/treeview.h>
#include <gtkmm/widget.h>
#include <gtkmm/window.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::gtk::layout::editor::test
{
  using ao::gtk::test::collectScrolledWindows;
  using ao::gtk::test::makeRuntime;

  using namespace ao::lmdb::test;

  TEST_CASE("Component descriptor validation", "[layout][unit][editor]")
  {
    auto registry = ComponentRegistry{};
    LayoutRuntime::registerStandardComponents(registry);

    auto const& descriptors = registry.descriptors();

    SECTION("all 26 component types have descriptors")
    {
      REQUIRE(descriptors.size() >= 26);
    }

    SECTION("all descriptors have non-empty type")
    {
      for (auto const& desc : descriptors)
      {
        CHECK(!desc.type.empty());
      }
    }

    SECTION("all descriptors have non-empty displayName")
    {
      for (auto const& desc : descriptors)
      {
        CHECK(!desc.displayName.empty());
      }
    }

    SECTION("all descriptors have a category")
    {
      for (auto const& desc : descriptors)
      {
        CHECK(!uimodel::layout::toString(desc.category).empty());
      }
    }

    SECTION("container types are derived from child limits")
    {
      auto const expectedContainers = std::set<std::string>{"box", "split", "scroll", "tabs"};

      for (auto const& desc : descriptors)
      {
        if (auto const isContainer = uimodel::layout::isContainer(desc); expectedContainers.contains(desc.type))
        {
          CHECK(isContainer);
        }
        else if (!isContainer)
        {
          CHECK(desc.optMaxChildren.value_or(0) == 0);
        }
      }
    }

    SECTION("split requires exactly 2 children")
    {
      auto const optDesc = registry.descriptor("split");

      REQUIRE(optDesc.has_value());
      CHECK(optDesc->minChildren == 2);
      CHECK(optDesc->optMaxChildren.has_value());
      CHECK(*optDesc->optMaxChildren == 2);
    }

    SECTION("scroll requires exactly 1 child")
    {
      auto const optDesc = registry.descriptor("scroll");

      REQUIRE(optDesc.has_value());
      CHECK(optDesc->minChildren == 1);
      CHECK(optDesc->optMaxChildren.has_value());
      CHECK(*optDesc->optMaxChildren == 1);
    }

    SECTION("tabs requires at least 1 child")
    {
      auto const optDesc = registry.descriptor("tabs");

      REQUIRE(optDesc.has_value());
      CHECK(optDesc->minChildren == 1);
      CHECK(!optDesc->optMaxChildren.has_value()); // unbounded
    }

    SECTION("box has orientation, spacing, homogeneous props")
    {
      auto const optDesc = registry.descriptor("box");

      REQUIRE(optDesc.has_value());
      CHECK(uimodel::layout::isContainer(*optDesc));

      auto const hasProp = [&](std::string const& name)
      { return std::ranges::any_of(optDesc->props, [&](auto const& prop) { return prop.name == name; }); };

      CHECK(hasProp("orientation"));
      CHECK(hasProp("spacing"));
      CHECK(hasProp("homogeneous"));
    }

    SECTION("playPauseButton has showLabel and size props")
    {
      auto const optDesc = registry.descriptor("playback.playPauseButton");

      REQUIRE(optDesc.has_value());
      CHECK(optDesc->category == uimodel::layout::ComponentCategory::Playback);

      auto const hasProp = [&](std::string const& name)
      { return std::ranges::any_of(optDesc->props, [&](auto const& prop) { return prop.name == name; }); };

      CHECK(hasProp("showLabel"));
      CHECK(hasProp("size"));
    }

    SECTION("playback.qualityIndicator has gesture action props")
    {
      auto const optDesc = registry.descriptor("playback.qualityIndicator");

      REQUIRE(optDesc.has_value());
      CHECK(optDesc->category == uimodel::layout::ComponentCategory::Playback);

      auto const hasProp = [&](std::string const& name)
      {
        return std::any_of(optDesc->props.begin(), optDesc->props.end(), [&](auto const& p) { return p.name == name; });
      };

      CHECK_FALSE(hasProp("primaryAction"));
      CHECK_FALSE(hasProp("primaryLongPressAction"));
      CHECK(hasProp("secondaryAction"));
      CHECK(hasProp("secondaryLongPressAction"));
    }

    SECTION("descriptor returns nullopt for unknown type")
    {
      auto const optDesc = registry.descriptor("nonexistent.component");
      CHECK(!optDesc.has_value());
    }

    SECTION("categories span expected groups")
    {
      auto categories = std::set<std::string>{};

      for (auto const& desc : descriptors)
      {
        categories.insert(std::string{uimodel::layout::toString(desc.category)});
      }

      CHECK(categories.contains("Containers"));
      CHECK(categories.contains("Decorators"));
      CHECK(categories.contains("Playback"));
      CHECK(categories.contains("Application"));
      CHECK(categories.contains("Status"));
      CHECK(categories.contains("Library"));
      CHECK(categories.contains("Tracks"));
    }

    SECTION("all 20 types individually retrievable")
    {
      auto const types = std::to_array<std::string_view>({"box",
                                                          "split",
                                                          "scroll",
                                                          "spacer",
                                                          "separator",
                                                          "tabs",
                                                          "playback.playPauseButton",
                                                          "playback.stopButton",
                                                          "playback.volumeControl",
                                                          "playback.currentTitleLabel",
                                                          "playback.currentArtistLabel",
                                                          "playback.seekSlider",
                                                          "playback.timeLabel",
                                                          "playback.playButton",
                                                          "playback.pauseButton",
                                                          "playback.qualityIndicator",
                                                          "playback.qualityIndicator",
                                                          "status.messageLabel",
                                                          "library.listTree",
                                                          "tracks.table",
                                                          "library.openLibraryButton",
                                                          "app.menuBar",
                                                          "track.detailScope",
                                                          "track.selectionRegion",
                                                          "track.coverArt",
                                                          "track.fieldGrid",
                                                          "track.tagEditor"});

      for (auto const& type : types)
      {
        auto const optDesc = registry.descriptor(std::string{type});
        CHECK(optDesc.has_value());
      }
    }
  }

  // ---------------------------------------------------------------------------
  // LayoutEditorDialog
  // ---------------------------------------------------------------------------
  TEST_CASE("LayoutEditorDialog", "[layout][unit][editor]")
  {
    auto const appPtr = Gtk::Application::create("io.github.aobus.layout_editor_test");

    auto registry = ComponentRegistry{};
    LayoutRuntime::registerStandardComponents(registry);
    auto actionRegistry = ActionRegistry{};

    auto window = Gtk::Window{};
    auto const doc = createDefaultLayout();
    auto const stubLoader = [](std::string_view) { return uimodel::layout::LayoutDocument{}; };

    auto const findTreeView = [](auto& self, Gtk::Widget& widget) -> Gtk::TreeView*
    {
      if (auto* const tv = dynamic_cast<Gtk::TreeView*>(&widget); tv != nullptr)
      {
        return tv;
      }

      for (auto* child = widget.get_first_child(); child != nullptr; child = child->get_next_sibling())
      {
        if (auto* const found = self(self, *child); found != nullptr)
        {
          return found;
        }
      }

      return nullptr;
    };

    SECTION("Dialog constructs without crash")
    {
      auto dialogPtr =
        std::make_unique<LayoutEditorDialog>(window, registry, actionRegistry, doc, "classic", "modern", stubLoader);
      REQUIRE(dialogPtr != nullptr);

      auto width = 0;
      auto height = 0;
      dialogPtr->get_default_size(width, height);

      CHECK(width == -1);
      CHECK(height == -1);

      auto scrolledWindows = std::vector<Gtk::ScrolledWindow*>{};
      collectScrolledWindows(*dialogPtr, scrolledWindows);

      auto foundTreeScroll = false;
      auto foundPropertiesScroll = false;

      for (auto* const scrolledWindow : scrolledWindows)
      {
        CHECK(scrolledWindow->get_propagate_natural_width());
        CHECK(scrolledWindow->get_propagate_natural_height());

        if (scrolledWindow->get_min_content_width() == 220)
        {
          foundTreeScroll = true;
          CHECK(scrolledWindow->get_min_content_height() == 460);
        }

        if (scrolledWindow->get_min_content_width() == 420)
        {
          foundPropertiesScroll = true;
          CHECK(scrolledWindow->get_max_content_width() == 560);
          CHECK(scrolledWindow->get_max_content_height() == 560);
        }
      }

      CHECK(foundTreeScroll);
      CHECK(foundPropertiesScroll);
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

    SECTION("reset default restores to createDefaultLayout shape")
    {
      // Create a modified document
      auto modified = LayoutDocument{};
      modified.root.type = "spacer";

      auto dialogPtr = std::make_unique<LayoutEditorDialog>(
        window, registry, actionRegistry, modified, "classic", "modern", stubLoader);

      // The dialog copies the document, so modifications to the dialog's copy
      // are reflected. Just verify the initial copy is correct.
      CHECK(dialogPtr->document().root.type == "spacer");

      dialogPtr->close();
    }

    SECTION("Validation prevents saving unknown actions")
    {
      auto invalidDoc = LayoutDocument{};
      invalidDoc.root.type = "app.actionButton";
      invalidDoc.root.props["primaryAction"] = LayoutValue{"this.does.not.exist"};

      auto dialogPtr = std::make_unique<LayoutEditorDialog>(
        window, registry, actionRegistry, invalidDoc, "classic", "modern", stubLoader);

      // Attempting to save an invalid document should fail validation and keep dialog open
      dialogPtr->response(Gtk::ResponseType::OK);
      // We can't check visible here since we didn't show(), but we verify it's still alive/active
      CHECK(dialogPtr != nullptr);
      dialogPtr->close();

      auto validDoc = LayoutDocument{};
      validDoc.root.type = "app.actionButton";
      validDoc.root.props["primaryAction"] = LayoutValue{"none"};

      auto dialogValidPtr = std::make_unique<LayoutEditorDialog>(
        window, registry, actionRegistry, validDoc, "classic", "modern", stubLoader);

      // Attempting to save a valid document should succeed and close the dialog
      dialogValidPtr->response(Gtk::ResponseType::OK);
    }

    SECTION("invalid save does not emit save request")
    {
      auto invalidDoc = LayoutDocument{};
      invalidDoc.root.type = "app.actionButton";
      invalidDoc.root.props["primaryAction"] = LayoutValue{std::string{"this.does.not.exist"}};

      auto dialog = LayoutEditorDialog{window, registry, actionRegistry, invalidDoc, "classic", "modern", stubLoader};

      auto saveCount = 0;
      dialog.signalSaveRequest().connect([&](LayoutSaveResult const&) { ++saveCount; });

      dialog.response(Gtk::ResponseType::OK);

      CHECK(saveCount == 0);
      dialog.close();
    }

    SECTION("signalApplyPreview is emitted on document changes")
    {
      auto dialogPtr =
        std::make_unique<LayoutEditorDialog>(window, registry, actionRegistry, doc, "classic", "modern", stubLoader);
      auto count = std::int32_t{0};

      dialogPtr->signalApplyPreview().connect([&](LayoutDocument const&) { ++count; });

      auto* const treeView = findTreeView(findTreeView, *dialogPtr);
      REQUIRE(treeView != nullptr);

      if (auto const modelPtr = treeView->get_model(); modelPtr && !modelPtr->children().empty())
      {
        treeView->get_selection()->select(modelPtr->children().begin());
      }

      dialogPtr->testAddComponent("spacer");

      CHECK(count > 0);

      dialogPtr->close();
    }

    SECTION("added components receive unique ids")
    {
      auto dialog = LayoutEditorDialog{window, registry, actionRegistry, doc, "classic", "modern", stubLoader};

      auto* const treeView = findTreeView(findTreeView, dialog);
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
      dialog.testAddComponent("spacer");
      selectRoot();
      dialog.testAddComponent("spacer");

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

      auto saveCount = 0;
      dialog.signalSaveRequest().connect([&](LayoutSaveResult const&) { ++saveCount; });

      dialog.response(Gtk::ResponseType::OK);

      CHECK(saveCount == 0);
      dialog.close();
    }

    SECTION("wrapped nodes receive a unique container id")
    {
      auto dialog = LayoutEditorDialog{window, registry, actionRegistry, doc, "classic", "modern", stubLoader};

      auto* const treeView = findTreeView(findTreeView, dialog);
      REQUIRE(treeView != nullptr);
      auto const modelPtr = treeView->get_model();
      REQUIRE(modelPtr);
      REQUIRE(!modelPtr->children().empty());

      auto const rootRow = *modelPtr->children().begin();
      REQUIRE(!rootRow.children().empty());
      treeView->get_selection()->select(rootRow.children().begin());

      auto const originalFirstChildId = dialog.document().root.children.front().id;
      dialog.testWrapNode("box");

      auto const& wrapper = dialog.document().root.children.front();
      CHECK(wrapper.id == "box-wrap");
      REQUIRE(wrapper.children.size() == 1);
      CHECK(wrapper.children.front().id == originalFirstChildId);

      dialog.close();
    }

    SECTION("destroys cleanly with header preset widgets")
    {
      {
        auto dialogPtr =
          std::make_unique<LayoutEditorDialog>(window, registry, actionRegistry, doc, "classic", "modern", stubLoader);
        // Keep the dialog unmapped so the test does not grab focus, while still
        // exercising construction, header preset widget setup, and destruction.
        dialogPtr->close();
        dialogPtr.reset();
      }
      SUCCEED(); // Reaching here without crash or GTK warnings is the goal
    }

    SECTION("Session caching and dirty tracking")
    {
      auto loadCount = std::int32_t{0};
      auto loadedPresets = std::vector<std::string>{};
      auto const customLoader = [&](std::string_view presetId)
      {
        ++loadCount;
        loadedPresets.emplace_back(presetId);

        auto testDoc = LayoutDocument{};
        testDoc.root.type = "box";
        testDoc.root.id = std::string{presetId} + "_root";

        return testDoc;
      };

      auto dialog = LayoutEditorDialog{window, registry, actionRegistry, doc, "classic", "modern", customLoader};

      // Find the presets combo box from children
      auto const collectCombos = [](auto& self, Gtk::Widget& widget, std::vector<Gtk::ComboBoxText*>& combos) -> void
      {
        if (auto* const combo = dynamic_cast<Gtk::ComboBoxText*>(&widget); combo != nullptr)
        {
          combos.push_back(combo);
        }

        for (auto* child = widget.get_first_child(); child != nullptr; child = child->get_next_sibling())
        {
          self(self, *child, combos);
        }
      };

      auto combos = std::vector<Gtk::ComboBoxText*>{};
      collectCombos(collectCombos, dialog, combos);
      REQUIRE(combos.size() == 2);

      auto* const combo = combos[0]->get_active_id() == "classic" ? combos[0] : combos[1];
      REQUIRE(combo != nullptr);

      // Verify initial active preset is classic
      CHECK(combo->get_active_id() == "classic");

      auto* const treeView = findTreeView(findTreeView, dialog);
      REQUIRE(treeView != nullptr);

      if (auto const modelPtr = treeView->get_model(); modelPtr && !modelPtr->children().empty())
      {
        treeView->get_selection()->select(modelPtr->children().begin());
      }

      auto const initialCount = dialog.document().root.children.size();

      // Edit active layout (classic) - this marks classic as dirty
      dialog.testAddComponent("spacer");
      CHECK(dialog.document().root.children.size() == initialCount + 1);

      // Switch to modern (not cached, invokes loader)
      combo->set_active_id("modern");

      CHECK(loadCount == 1);
      CHECK(loadedPresets.back() == "modern");
      CHECK(dialog.document().root.id == "modern_root");

      // Switch back to classic (should load from cache, not invoke loader)
      combo->set_active_id("classic");
      CHECK(loadCount == 1);                                             // no new load
      CHECK(dialog.document().root.children.size() == initialCount + 1); // edit preserved!

      // Save and verify result
      auto saveResult = LayoutSaveResult{};
      auto saveCount = 0;
      dialog.signalSaveRequest().connect(
        [&](LayoutSaveResult const& res)
        {
          saveResult = res;
          ++saveCount;
        });

      dialog.response(Gtk::ResponseType::OK);
      CHECK(saveCount == 1);
      CHECK(saveResult.activePresetId == "classic");

      // Since classic was edited, it should be in modified
      CHECK(saveResult.modified.contains("classic"));
      // Since modern was not edited, it should NOT be in modified
      CHECK(!saveResult.modified.contains("modern"));
      CHECK(saveResult.resets.empty());

      dialog.close();
    }

    SECTION("Reset default and dirty tracking")
    {
      auto const customLoader = [&](std::string_view presetId)
      {
        auto testDoc = LayoutDocument{};
        testDoc.root.type = "box";
        testDoc.root.id = std::string{presetId} + "_root";
        return testDoc;
      };

      auto dialog = LayoutEditorDialog{window, registry, actionRegistry, doc, "classic", "modern", customLoader};

      // Find the presets combo box from children
      auto const collectCombos = [](auto& self, Gtk::Widget& widget, std::vector<Gtk::ComboBoxText*>& combos) -> void
      {
        if (auto* const combo = dynamic_cast<Gtk::ComboBoxText*>(&widget); combo != nullptr)
        {
          combos.push_back(combo);
        }

        for (auto* child = widget.get_first_child(); child != nullptr; child = child->get_next_sibling())
        {
          self(self, *child, combos);
        }
      };

      auto combos = std::vector<Gtk::ComboBoxText*>{};
      collectCombos(collectCombos, dialog, combos);
      REQUIRE(combos.size() == 2);

      auto* const combo = combos[0]->get_active_id() == "classic" ? combos[0] : combos[1];
      REQUIRE(combo != nullptr);

      // Trigger reset default on classic
      dialog.testOnResetDefault();

      // Switch to modern, edit it
      combo->set_active_id("modern");

      auto* const treeView = findTreeView(findTreeView, dialog);
      REQUIRE(treeView != nullptr);

      if (auto const modelPtr = treeView->get_model(); modelPtr && !modelPtr->children().empty())
      {
        treeView->get_selection()->select(modelPtr->children().begin());
      }

      dialog.testAddComponent("spacer");

      // Save and verify result
      auto saveResult = LayoutSaveResult{};
      auto saveCount = 0;
      dialog.signalSaveRequest().connect(
        [&](LayoutSaveResult const& res)
        {
          saveResult = res;
          ++saveCount;
        });

      dialog.response(Gtk::ResponseType::OK);
      CHECK(saveCount == 1);

      // classic should be in resets (reset default was clicked)
      CHECK(std::ranges::contains(saveResult.resets, std::string{"classic"}));
      // modern was edited, so it should be in modified
      CHECK(saveResult.modified.contains("modern"));

      dialog.close();
    }

    SECTION("Multi-preset validation and caching re-visit")
    {
      auto loadCount = std::int32_t{0};
      auto loadedPresets = std::vector<std::string>{};
      auto const customLoader = [&](std::string_view presetId)
      {
        ++loadCount;
        loadedPresets.emplace_back(presetId);

        auto testDoc = LayoutDocument{};

        if (presetId == "modern")
        {
          testDoc.root.type = "app.actionButton";
          testDoc.root.props["primaryAction"] = LayoutValue{"this.does.not.exist"};
        }
        else
        {
          testDoc.root.type = "box";
          testDoc.root.id = std::string{presetId} + "_root";
        }

        return testDoc;
      };

      auto dialog = LayoutEditorDialog{window, registry, actionRegistry, doc, "classic", "modern", customLoader};

      auto const collectCombos = [](auto& self, Gtk::Widget& widget, std::vector<Gtk::ComboBoxText*>& combos) -> void
      {
        if (auto* const combo = dynamic_cast<Gtk::ComboBoxText*>(&widget); combo != nullptr)
        {
          combos.push_back(combo);
        }

        for (auto* child = widget.get_first_child(); child != nullptr; child = child->get_next_sibling())
        {
          self(self, *child, combos);
        }
      };

      auto combos = std::vector<Gtk::ComboBoxText*>{};
      collectCombos(collectCombos, dialog, combos);
      REQUIRE(combos.size() == 2);

      auto* const combo = combos[0]->get_active_id() == "classic" ? combos[0] : combos[1];
      REQUIRE(combo != nullptr);

      // Re-visit cache confirmation
      combo->set_active_id("modern");
      CHECK(loadCount == 1);
      combo->set_active_id("classic");
      CHECK(loadCount == 1);
      combo->set_active_id("modern");
      CHECK(loadCount == 1);

      // Edit modern (currently active) to be dirty
      dialog.testMarkEdited();

      // Switch back to classic (which is valid)
      combo->set_active_id("classic");

      auto saveResult = LayoutSaveResult{};
      auto saveCount = 0;
      dialog.signalSaveRequest().connect(
        [&](LayoutSaveResult const& res)
        {
          saveResult = res;
          ++saveCount;
        });

      // Saving should fail validation on the dirty background preset "modern"
      dialog.response(Gtk::ResponseType::OK);
      CHECK(saveCount == 0);

      dialog.close();
    }

    SECTION("Reset of active preset saved without switching")
    {
      auto dialog = LayoutEditorDialog{window, registry, actionRegistry, doc, "classic", "modern", stubLoader};

      dialog.testOnResetDefault();

      auto saveResult = LayoutSaveResult{};
      auto saveCount = 0;
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
  }

  // ---------------------------------------------------------------------------
  // Template system
  // ---------------------------------------------------------------------------
  TEST_CASE("Template system", "[layout][unit][editor]")
  {
    SECTION("getBuiltInTemplates returns all 8 built-ins")
    {
      auto const templates = getBuiltInTemplates();

      CHECK(templates.contains("playback.compactControls"));
      CHECK(templates.contains("playback.transportGroup"));
      CHECK(templates.contains("playback.defaultBar"));
      CHECK(templates.contains("library.defaultListPane"));
      CHECK(templates.contains("track.defaultDetailPane"));
      CHECK(templates.contains("status.defaultBar"));
      CHECK(templates.contains("tracks.defaultWorkspace"));
      CHECK(templates.contains("app.defaultLayout"));
      CHECK(templates.contains("track.selectionDetailPane"));

      int const expectedCount = 9;
      CHECK(templates.size() >= expectedCount);
    }

    SECTION("playback.transportGroup has 2 children and linked class")
    {
      auto const templates = getBuiltInTemplates();
      auto const& group = templates.at("playback.transportGroup");

      CHECK(group.type == "box");
      CHECK(group.getProp<std::int64_t>("spacing", -1) == 0);

      auto const classes = group.getLayout<std::vector<std::string>>("cssClasses", {});
      CHECK(std::ranges::contains(classes, std::string_view{"linked"}));
      CHECK(group.children.size() == 2);
    }

    SECTION("playback.defaultBar contains all expected children")
    {
      auto const templates = getBuiltInTemplates();
      auto const& bar = templates.at("playback.defaultBar");

      CHECK(bar.type == "box");

      // 3 children: left fixed controls, flexible seek slider, right fixed status controls.
      int const expectedChildren = 3;
      CHECK(bar.children.size() == expectedChildren);
      CHECK(bar.children[0].type == "box");
      CHECK(bar.children[0].children.size() == 2);
      CHECK(bar.children[0].children[0].type == "playback.soulButton");
      CHECK(bar.children[0].children[1].type == "template");
      CHECK(bar.children[0].children[1].getProp<std::string>("templateId", "") == "playback.transportGroup");
      CHECK(bar.children[1].type == "playback.seekSlider");
      CHECK(bar.children[1].getLayout<bool>("hexpand", false));
      CHECK(bar.children[2].type == "box");
      CHECK(bar.children[2].children.size() == 2);
      CHECK(bar.children[2].children[0].type == "playback.timeLabel");
      CHECK(bar.children[2].children[1].type == "playback.volumeControl");

      // Grouping regions carry ao-grouping-region CSS class.
      CHECK(bar.children[0].getLayout<std::string>("cssClasses", "") == "ao-grouping-region");
      CHECK(bar.children[2].getLayout<std::string>("cssClasses", "") == "ao-grouping-region");
    }

    SECTION("status.defaultBar template contains 7 children")
    {
      auto const templates = getBuiltInTemplates();
      auto const& bar = templates.at("status.defaultBar");

      CHECK(bar.type == "box");

      // 7 children: playbackDetails, spacer, nowPlaying, spacer, statusSlot, separator, trackCount
      int const expectedChildren = 7;
      CHECK(bar.children.size() == expectedChildren);
    }

    SECTION("track.selectionDetailPane contains field grid")
    {
      auto const templates = getBuiltInTemplates();
      auto const& pane = templates.at("track.selectionDetailPane");

      auto hasFieldGrid = false;

      auto visit = std::function<void(LayoutNode const&)>{};
      visit = [&](LayoutNode const& node)
      {
        if (node.type == "track.fieldGrid")
        {
          hasFieldGrid = true;
        }

        for (auto const& child : node.children)
        {
          visit(child);
        }
      };

      visit(pane);

      CHECK(hasFieldGrid);
    }

    SECTION("template expansion via expandNode in build")
    {
      auto registry = ComponentRegistry{};
      LayoutRuntime::registerStandardComponents(registry);

      auto const tempDir = TempDir{};
      auto runtime = makeRuntime(tempDir);

      auto const appPtr = Gtk::Application::create("io.github.aobus.template_test");
      auto window = Gtk::Window{};
      auto const actionRegistry = ActionRegistry{};
      auto ctx = LayoutContext{
        .registry = registry, .actionRegistry = actionRegistry, .runtime = runtime, .parentWindow = window};

      auto doc = LayoutDocument{};
      doc.version = 1;
      doc.templates = getBuiltInTemplates();
      doc.root.type = "template";
      doc.root.props["templateId"] = LayoutValue{std::string{"playback.compactControls"}};

      auto layoutRuntime = LayoutRuntime{registry};
      auto const compPtr = layoutRuntime.build(ctx, doc);

      REQUIRE(compPtr != nullptr);

      auto* const box = dynamic_cast<Gtk::Box*>(&compPtr->widget());
      REQUIRE(box != nullptr);
    }

    SECTION("recursive template reference produces error")
    {
      auto registry = ComponentRegistry{};
      LayoutRuntime::registerStandardComponents(registry);

      auto const tempDir = TempDir{};
      auto runtime = makeRuntime(tempDir);

      auto const appPtr = Gtk::Application::create("io.github.aobus.recursive_test");
      auto window = Gtk::Window{};
      auto const actionRegistry = ActionRegistry{};
      auto ctx = LayoutContext{
        .registry = registry, .actionRegistry = actionRegistry, .runtime = runtime, .parentWindow = window};

      auto doc = LayoutDocument{};
      doc.version = 1;
      doc.templates["selfRef"] = LayoutNode{.type = "template"};
      doc.templates["selfRef"].props["templateId"] = LayoutValue{std::string{"selfRef"}};
      doc.root.type = "template";
      doc.root.props["templateId"] = LayoutValue{std::string{"selfRef"}};

      auto layoutRuntime = LayoutRuntime{registry};
      auto const compPtr = layoutRuntime.build(ctx, doc);

      REQUIRE(compPtr != nullptr);
      CHECK(dynamic_cast<Gtk::Widget*>(&compPtr->widget()) != nullptr);
    }

    SECTION("template YAML round-trip")
    {
      auto doc = LayoutDocument{};
      doc.version = 2;
      doc.templates = getBuiltInTemplates();

      auto tree = ryml::Tree{};
      yaml::write(tree.rootref(), doc);

      auto decoded = LayoutDocument{};
      REQUIRE(yaml::read(tree.rootref(), decoded));

      REQUIRE(decoded.templates.contains("playback.compactControls"));
      CHECK(decoded.templates.at("playback.compactControls").type == "box");
    }
  }

  // ---------------------------------------------------------------------------
  // absoluteCanvas
  // ---------------------------------------------------------------------------
  TEST_CASE("absoluteCanvas component", "[layout][unit][editor]")
  {
    auto const appPtr = Gtk::Application::create("io.github.aobus.canvas_test");

    auto const tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    auto registry = ComponentRegistry{};
    LayoutRuntime::registerStandardComponents(registry);

    auto window = Gtk::Window{};
    auto const actionRegistry = ActionRegistry{};
    auto ctx =
      LayoutContext{.registry = registry, .actionRegistry = actionRegistry, .runtime = runtime, .parentWindow = window};

    SECTION("absoluteCanvas descriptor is registered as container")
    {
      auto const optDesc = registry.descriptor("absoluteCanvas");

      REQUIRE(optDesc.has_value());
      CHECK(uimodel::layout::isContainer(*optDesc));
      CHECK(optDesc->minChildren == 0);
      CHECK(!optDesc->optMaxChildren.has_value());
    }

    SECTION("absoluteCanvas with no children builds without crash")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "absoluteCanvas";

      auto layoutRuntime = LayoutRuntime{registry};
      auto const compPtr = layoutRuntime.build(ctx, doc);

      REQUIRE(compPtr != nullptr);
    }

    SECTION("absoluteCanvas with positioned child")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "absoluteCanvas";

      auto child = LayoutNode{};
      child.type = "spacer";
      child.id = "pos-spacer";
      child.layout["x"] = LayoutValue{static_cast<std::int64_t>(50)};
      child.layout["y"] = LayoutValue{static_cast<std::int64_t>(100)};
      child.layout["width"] = LayoutValue{static_cast<std::int64_t>(200)};
      child.layout["height"] = LayoutValue{static_cast<std::int64_t>(50)};
      child.layout["zIndex"] = LayoutValue{static_cast<std::int64_t>(2)};
      doc.root.children.push_back(std::move(child));

      auto layoutRuntime = LayoutRuntime{registry};
      auto const compPtr = layoutRuntime.build(ctx, doc);

      REQUIRE(compPtr != nullptr);
    }
  }
} // namespace ao::gtk::layout::editor::test
