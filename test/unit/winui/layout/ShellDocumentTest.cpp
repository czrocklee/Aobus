// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/winui/layout/ShellDocument.h>

#include <ao/Error.h>
#include <ao/uimodel/layout/action/LayoutActionSlot.h>
#include <ao/uimodel/layout/action/LayoutActionSlotResolution.h>
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/layout/document/LayoutPreparation.h>
#include <ao/winui/layout/LayoutCatalog.h>
#include <ao/winui/layout/ThemeSurface.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::winui::test
{
  using uimodel::LayoutNode;

  namespace
  {
    std::string readShippedDocument(ShellPreset const preset)
    {
      auto const path = std::filesystem::path{AOBUS_WINDOWS_LAYOUT_DIR} / shellPresetResource(preset);
      auto stream = std::ifstream{path, std::ios::binary};
      REQUIRE(stream.is_open());
      return std::string{std::istreambuf_iterator{stream}, std::istreambuf_iterator<char>{}};
    }

    /// The frame source that registers actions and composes the shells' menus.
    std::string readShellBuilderSource()
    {
      auto stream = std::ifstream{std::filesystem::path{AOBUS_WINDOWS_SHELL_BUILDER_SOURCE}, std::ios::binary};
      REQUIRE(stream.is_open());
      return std::string{std::istreambuf_iterator{stream}, std::istreambuf_iterator<char>{}};
    }

    void collectIds(LayoutNode const& node, std::vector<std::string>& ids)
    {
      if (!node.id.empty())
      {
        ids.push_back(node.id);
      }

      for (auto const& child : node.children)
      {
        collectIds(child, ids);
      }
    }

    /// Every node that shows a status reading, either kind.
    void collectStatusReadings(LayoutNode const& node, std::vector<LayoutNode const*>& readings)
    {
      if (node.type == "status.trackCount" || node.type == "status.selectionInfo")
      {
        readings.push_back(&node);
      }

      for (auto const& child : node.children)
      {
        collectStatusReadings(child, readings);
      }
    }

    std::vector<std::string> nodeIds(uimodel::PreparedLayout const& layout)
    {
      auto ids = std::vector<std::string>{};
      collectIds(layout.effectiveRoot(), ids);
      return ids;
    }

    bool contains(std::vector<std::string> const& ids, std::string_view const id)
    {
      return std::ranges::contains(ids, id);
    }

    /// Whether @p node is a pane that sizes itself from the persisted Windows settings.
    bool ownsPersistedWidth(LayoutNode const& node)
    {
      if (node.type == "windows.inspectorPane")
      {
        return true;
      }

      // Only the tree presentation occupies a slot of its own; a NavigationView
      // draws its pane inside the region it is given.
      return node.type == "windows.navigationPane" && node.propertyOr<std::string>("presentation", {}) == "tree";
    }

    void collectSelfSizedPanes(LayoutNode const& node,
                               std::string_view const parentType,
                               std::vector<std::pair<LayoutNode const*, std::string>>& panes)
    {
      if (ownsPersistedWidth(node))
      {
        panes.emplace_back(&node, std::string{parentType});
      }

      for (auto const& child : node.children)
      {
        collectSelfSizedPanes(child, node.type, panes);
      }
    }

    /// Whether @p node runs play/pause when the user clicks it.
    bool isPlayPauseControl(uimodel::LayoutComponentCatalog const& catalog, LayoutNode const& node)
    {
      if (node.type == "playback.transportButton")
      {
        return node.propertyOr<std::string>("command", "playPause") == "playPause";
      }

      if (node.type != "playback.soulButton")
      {
        return false;
      }

      // The soul plays and pauses on its own unless the document spends that
      // gesture on an action, which is the only thing that can take it away.
      auto const optDescriptor = catalog.descriptor(node.type);
      return optDescriptor &&
             !isActionSlotBound(optDescriptor->actionPolicy, node, uimodel::LayoutActionSlot::PrimaryClick);
    }

    std::size_t countPlayPauseControls(uimodel::LayoutComponentCatalog const& catalog, LayoutNode const& node)
    {
      auto total = isPlayPauseControl(catalog, node) ? std::size_t{1} : std::size_t{0};

      for (auto const& child : node.children)
      {
        total += countPlayPauseControls(catalog, child);
      }

      return total;
    }

    void collectByType(LayoutNode const& node, std::string_view const type, std::vector<LayoutNode const*>& found)
    {
      if (node.type == type)
      {
        found.push_back(&node);
      }

      for (auto const& child : node.children)
      {
        collectByType(child, type, found);
      }
    }

    /// Every action id the shipped document actually binds, defaults included.
    void collectBoundActions(uimodel::LayoutComponentCatalog const& catalog,
                             LayoutNode const& node,
                             std::vector<std::string>& actions)
    {
      if (auto const optDescriptor = catalog.descriptor(node.type); optDescriptor)
      {
        for (auto const slot : {uimodel::LayoutActionSlot::PrimaryClick,
                                uimodel::LayoutActionSlot::PrimaryLongPress,
                                uimodel::LayoutActionSlot::SecondaryClick,
                                uimodel::LayoutActionSlot::SecondaryLongPress})
        {
          if (!isActionSlotBound(optDescriptor->actionPolicy, node, slot))
          {
            continue;
          }

          auto const it = node.props.find(actionPropForSlot(slot));
          auto const* const authored = it == node.props.end() ? nullptr : it->second.getIf<std::string>();
          actions.emplace_back(authored != nullptr && !authored->empty()
                                 ? *authored
                                 : std::string{optDescriptor->actionPolicy.defaultAction(slot)});
        }
      }

      for (auto const& child : node.children)
      {
        collectBoundActions(catalog, child, actions);
      }
    }

    void collectSurfaces(LayoutNode const& node, std::vector<ThemeSurface>& found)
    {
      if (auto const optSurface = planThemeSurface(node); optSurface)
      {
        found.push_back(*optSurface);
      }

      for (auto const& child : node.children)
      {
        collectSurfaces(child, found);
      }
    }
  } // namespace

  TEST_CASE("shellPresetResource - the WinUI build packages every preset where the shell reads them",
            "[winui][unit][layout]")
  {
    // A preset that is not packaged leaves the shell with nothing to build, and
    // one packaged under another name is the same failure wearing a disguise.
    // Neither shows up until the app is started, so the pairing is checked here.
    auto stream = std::ifstream{std::filesystem::path{AOBUS_WINDOWS_WINUI_CMAKE}, std::ios::binary};
    REQUIRE(stream.is_open());
    auto const project = std::string{std::istreambuf_iterator{stream}, std::istreambuf_iterator<char>{}};

    // MSBuild spells the packaged path with escaped backslashes; the shared
    // constant spells it with forward ones.
    auto folder = std::string{kShellPresetFolder};
    auto const separator = folder.find('/');
    REQUIRE(separator != std::string::npos);
    folder.replace(separator, 1, "\\\\");

    for (auto const preset : {ShellPreset::Modern, ShellPreset::Classic})
    {
      auto const resource = shellPresetResource(preset);
      INFO("preset " << resource);
      CHECK(project.contains(std::format("Link={}\\\\{}\"", folder, resource)));
    }
  }

  TEST_CASE("prepareShellPresetDocument - the shell registers every action the shipped presets bind",
            "[winui][unit][layout]")
  {
    // An action a preset binds but the shell never registers rejects the whole
    // candidate, which on the shipped path means a window with no shell in it.
    // Nothing but starting the app would otherwise say so.
    auto const builder = readShellBuilderSource();
    auto const& catalog = layoutCatalog();

    for (auto const preset : {ShellPreset::Modern, ShellPreset::Classic})
    {
      auto const resource = shellPresetResource(preset);
      INFO("preset " << resource);

      auto const prepared = prepareShellPresetDocument(readShippedDocument(preset), resource);
      REQUIRE(prepared.has_value());

      auto actions = std::vector<std::string>{};
      collectBoundActions(catalog, prepared->effectiveRoot(), actions);
      CHECK_FALSE(actions.empty());

      for (auto const& action : actions)
      {
        INFO("action " << action);
        CHECK(builder.contains(std::format("\"{}\"", action)));
      }
    }
  }

  TEST_CASE("prepareShellPresetDocument - every shell can ask for the inspector it hides", "[winui][unit][layout]")
  {
    /*
     * Below the wide tier the inspector is an overlay, which shows nothing
     * until it is asked for, so a shell with no way to ask cannot read track
     * details at any width it does not seat the pane inline. Modern asks from
     * its own document; Classic authors no toggle of its own and reaches the
     * command through the menu bar the frame composes for it.
     */
    auto const prepared =
      prepareShellPresetDocument(readShippedDocument(ShellPreset::Modern), shellPresetResource(ShellPreset::Modern));
    REQUIRE(prepared.has_value());

    auto actions = std::vector<std::string>{};
    collectBoundActions(layoutCatalog(), prepared->effectiveRoot(), actions);
    CHECK(contains(actions, "shell.toggleInspector"));

    auto const builder = readShellBuilderSource();
    CHECK(builder.contains("\"TrackDetailsMenuItem\""));
    CHECK(builder.contains("commands.toggleInspector"));
  }

  TEST_CASE("prepareShellPresetDocument - both shipped presets parse, expand, and validate", "[winui][unit][layout]")
  {
    for (auto const preset : {ShellPreset::Modern, ShellPreset::Classic})
    {
      auto const resource = shellPresetResource(preset);
      INFO("preset " << resource);

      auto const prepared = prepareShellPresetDocument(readShippedDocument(preset), resource);

      REQUIRE(prepared.has_value());
      CHECK_FALSE(prepared->effectiveRoot().children.empty());
    }
  }

  TEST_CASE("prepareShellPresetDocument - every preset paints the surfaces its theme section describes",
            "[winui][unit][layout]")
  {
    // A theme section that no node claims is a colour the user can set and
    // never see, so each preset must reach every slot its own half names.
    for (auto const& [preset, expected] : {std::pair{ShellPreset::Modern,
                                                     std::vector{ThemeSurface::Surface,
                                                                 ThemeSurface::ModernNavigation,
                                                                 ThemeSurface::ModernInspector,
                                                                 ThemeSurface::ModernNowPlaying}},
                                           std::pair{ShellPreset::Classic,
                                                     std::vector{ThemeSurface::Surface,
                                                                 ThemeSurface::ClassicToolbar,
                                                                 ThemeSurface::ClassicTree,
                                                                 ThemeSurface::ClassicStatusBar}}})
    {
      auto const resource = shellPresetResource(preset);
      INFO("preset " << resource);

      auto const prepared = prepareShellPresetDocument(readShippedDocument(preset), resource);
      REQUIRE(prepared.has_value());

      auto painted = std::vector<ThemeSurface>{};
      collectSurfaces(prepared->effectiveRoot(), painted);

      for (auto const surface : expected)
      {
        INFO("surface " << toString(surface));
        CHECK(std::ranges::find(painted, surface) != painted.end());
      }
    }
  }

  TEST_CASE("prepareShellPresetDocument - each preset carries the components the shell reconciles",
            "[winui][unit][layout]")
  {
    SECTION("modern")
    {
      auto const prepared =
        prepareShellPresetDocument(readShippedDocument(ShellPreset::Modern), shellPresetResource(ShellPreset::Modern));
      REQUIRE(prepared.has_value());

      auto const ids = nodeIds(*prepared);
      CHECK(contains(ids, "modern-track-table"));
      CHECK(contains(ids, "modern-track-detail"));
      // The inspector shows the selection's artwork above its fields, which is
      // a component of its own rather than part of the detail region.
      CHECK(contains(ids, "modern-inspector-cover"));
      CHECK(contains(ids, "modern-navigation"));
      CHECK(contains(ids, "modern-inspector"));
      CHECK(contains(ids, "modern-title-bar"));
    }

    SECTION("classic")
    {
      auto const prepared = prepareShellPresetDocument(
        readShippedDocument(ShellPreset::Classic), shellPresetResource(ShellPreset::Classic));
      REQUIRE(prepared.has_value());

      auto const ids = nodeIds(*prepared);
      CHECK(contains(ids, "classic-track-table"));
      CHECK(contains(ids, "classic-track-detail"));
      // Classic shows the selection's artwork above its property rows too; the
      // shells differ in how much room they give it, not in whether it is there.
      CHECK(contains(ids, "classic-inspector-cover"));
      CHECK(contains(ids, "classic-navigation"));
      CHECK(contains(ids, "classic-inspector"));
      CHECK(contains(ids, "classic-menu-bar"));
      CHECK(contains(ids, "classic-status-bar"));
    }
  }

  TEST_CASE("prepareShellPresetDocument - a pane that owns its width is never given a proportional slot",
            "[winui][unit][layout]")
  {
    // A `split` keeps its share of the axis whatever the child does with it, so
    // a pane that resizes and collapses itself would leave a hole behind. Those
    // panes belong in a `box`, which sizes a slot from the child unless the
    // child asked to expand.
    for (auto const preset : {ShellPreset::Modern, ShellPreset::Classic})
    {
      auto const resource = shellPresetResource(preset);
      INFO("preset " << resource);

      auto const prepared = prepareShellPresetDocument(readShippedDocument(preset), resource);
      REQUIRE(prepared.has_value());

      auto panes = std::vector<std::pair<LayoutNode const*, std::string>>{};
      collectSelfSizedPanes(prepared->effectiveRoot(), {}, panes);
      CHECK_FALSE(panes.empty());

      for (auto const& [node, parentType] : panes)
      {
        INFO("pane " << node->id);
        CHECK(parentType != "split");
        CHECK(parentType != "collapsibleSplit");
        CHECK_FALSE(node->layoutOr<bool>("hexpand", false));
      }
    }
  }

  TEST_CASE("prepareShellPresetDocument - only a browser summary yields its space below the wide tier",
            "[winui][unit][layout]")
  {
    // The Windows desktop shell specification has the browser summary yield to
    // the filter below the wide tier, and the component answers that by
    // collapsing itself. A status bar's own reading must never take the same
    // rule, so which of the two a node is has to be authored rather than
    // inferred from the type.
    auto const modern =
      prepareShellPresetDocument(readShippedDocument(ShellPreset::Modern), shellPresetResource(ShellPreset::Modern));
    REQUIRE(modern.has_value());

    auto const classic =
      prepareShellPresetDocument(readShippedDocument(ShellPreset::Classic), shellPresetResource(ShellPreset::Classic));
    REQUIRE(classic.has_value());

    auto readings = std::vector<LayoutNode const*>{};
    collectStatusReadings(modern->effectiveRoot(), readings);
    CHECK(readings.size() == 2);

    for (auto const* const node : readings)
    {
      INFO("node " << node->id);
      CHECK(node->propertyOr<std::string>("variant", {}) == kSummaryVariant);
    }

    readings.clear();
    collectStatusReadings(classic->effectiveRoot(), readings);
    CHECK_FALSE(readings.empty());

    for (auto const* const node : readings)
    {
      INFO("node " << node->id);
      CHECK(node->propertyOr<std::string>("variant", std::string{kStatusVariant}) == kStatusVariant);
    }
  }

  TEST_CASE("prepareShellPresetDocument - every preset offers play/pause exactly once", "[winui][unit][layout]")
  {
    // The soul is a transport control in one shell and the output device
    // affordance in the other, and nothing but its primary action slot says
    // which. Counting the ways a preset reaches play/pause catches both
    // mistakes: a shell that lost the gesture, and one where two controls
    // compete for the same click.
    auto const catalog = layoutCatalog();

    for (auto const preset : {ShellPreset::Modern, ShellPreset::Classic})
    {
      auto const resource = shellPresetResource(preset);
      INFO("preset " << resource);

      auto const prepared = prepareShellPresetDocument(readShippedDocument(preset), resource);
      REQUIRE(prepared.has_value());

      CHECK(countPlayPauseControls(catalog, prepared->effectiveRoot()) == 1);

      auto souls = std::vector<LayoutNode const*>{};
      collectByType(prepared->effectiveRoot(), "playback.soulButton", souls);
      CHECK_FALSE(souls.empty());

      auto const optDescriptor = catalog.descriptor("playback.soulButton");
      REQUIRE(optDescriptor);

      for (auto const* const soul : souls)
      {
        INFO("soul " << soul->id);
        // The gestures that compete with nothing the soul does are the shell's
        // everywhere, so no preset has to remember to author them.
        CHECK(isActionSlotBound(optDescriptor->actionPolicy, *soul, uimodel::LayoutActionSlot::SecondaryClick));
        CHECK(isActionSlotBound(optDescriptor->actionPolicy, *soul, uimodel::LayoutActionSlot::PrimaryLongPress));
      }
    }
  }

  TEST_CASE("prepareShellPresetDocument - the presets keep disjoint node ids so state never crosses shells",
            "[winui][unit][layout]")
  {
    auto const modern =
      prepareShellPresetDocument(readShippedDocument(ShellPreset::Modern), shellPresetResource(ShellPreset::Modern));
    auto const classic =
      prepareShellPresetDocument(readShippedDocument(ShellPreset::Classic), shellPresetResource(ShellPreset::Classic));
    REQUIRE(modern.has_value());
    REQUIRE(classic.has_value());

    CHECK(shellPresetId(ShellPreset::Modern) != shellPresetId(ShellPreset::Classic));

    for (auto const& id : nodeIds(*modern))
    {
      INFO("modern id " << id);
      CHECK_FALSE(contains(nodeIds(*classic), id));
    }
  }

  TEST_CASE("prepareShellPresetDocument - a defective document is rejected as a whole", "[winui][unit][layout]")
  {
    SECTION("unparsable YAML")
    {
      auto const prepared = prepareShellPresetDocument("version: 1\nroot: [unterminated", "broken.yaml");

      REQUIRE_FALSE(prepared.has_value());
      CHECK(prepared.error().code == Error::Code::FormatRejected);
      CHECK(prepared.error().message.contains("broken.yaml"));
    }

    SECTION("an unsupported document version")
    {
      auto const prepared = prepareShellPresetDocument("version: 2\nroot:\n  type: box\n", "future.yaml");

      REQUIRE_FALSE(prepared.has_value());
      CHECK(prepared.error().code == Error::Code::NotSupported);
    }

    SECTION("a component the Windows catalog does not register")
    {
      auto const prepared = prepareShellPresetDocument("version: 1\nroot:\n  id: root\n  type: tabs\n", "gtk.yaml");

      REQUIRE_FALSE(prepared.has_value());
      CHECK(prepared.error().code == Error::Code::FormatRejected);
      CHECK(prepared.error().message.contains("gtk.yaml"));
      CHECK(prepared.error().message.contains("unknown component type"));
    }

    SECTION("a document over the shipped size budget")
    {
      auto const oversized = std::string(uimodel::LayoutDocumentLimits::kDefaultMaxFileBytes + 1, 'x');
      auto const prepared = prepareShellPresetDocument(oversized, "huge.yaml");

      REQUIRE_FALSE(prepared.has_value());
      CHECK(prepared.error().code == Error::Code::ValueTooLarge);
    }
  }
} // namespace ao::winui::test
