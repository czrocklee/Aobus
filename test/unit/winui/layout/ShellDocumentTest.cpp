// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/winui/layout/ShellDocument.h>

#include <ao/Error.h>
#include <ao/uimodel/layout/component/LayoutSchema.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/layout/document/LayoutPreparation.h>
#include <ao/winui/layout/LayoutSchema.h>
#include <ao/winui/layout/ShellCommands.h>
#include <ao/winui/layout/ThemeSurface.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <ios>
#include <iterator>
#include <set>
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

    LayoutNode const* findById(LayoutNode const& node, std::string_view const id)
    {
      if (node.id == id)
      {
        return &node;
      }

      for (auto const& child : node.children)
      {
        if (auto const* found = findById(child, id); found != nullptr)
        {
          return found;
        }
      }

      return nullptr;
    }

    /// Whether @p node is a pane that sizes itself from the persisted Windows settings.
    bool hasPersistedWidth(LayoutNode const& node)
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
      if (hasPersistedWidth(node))
      {
        panes.emplace_back(&node, std::string{parentType});
      }

      for (auto const& child : node.children)
      {
        collectSelfSizedPanes(child, node.type, panes);
      }
    }

    /// Whether @p node runs play/pause when the user clicks it.
    bool isPlayPauseControl(uimodel::LayoutSchema const& schema, LayoutNode const& node)
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
      auto const optComponent = schema.component(node.type);
      return optComponent && !optComponent->actionId(node, uimodel::ActionSlot::PrimaryClick);
    }

    std::size_t countPlayPauseControls(uimodel::LayoutSchema const& schema, LayoutNode const& node)
    {
      auto total = isPlayPauseControl(schema, node) ? std::size_t{1} : std::size_t{0};

      for (auto const& child : node.children)
      {
        total += countPlayPauseControls(schema, child);
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
    void collectBoundActions(uimodel::LayoutSchema const& schema,
                             LayoutNode const& node,
                             std::vector<std::string>& actions)
    {
      if (auto const optComponent = schema.component(node.type); optComponent)
      {
        for (auto const slot : {uimodel::ActionSlot::PrimaryClick,
                                uimodel::ActionSlot::PrimaryLongPress,
                                uimodel::ActionSlot::SecondaryClick,
                                uimodel::ActionSlot::SecondaryLongPress})
        {
          auto const optActionId = optComponent->actionId(node, slot);

          if (!optActionId)
          {
            continue;
          }

          actions.emplace_back(*optActionId);
        }
      }

      for (auto const& child : node.children)
      {
        collectBoundActions(schema, child, actions);
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

  TEST_CASE("shellPresetResource - project declarations pair both preset files with shell paths",
            "[winui][unit][layout]")
  {
    // Source-level CMake/reader-name pairing only. MSBuild deployment and the
    // executable's files must still be checked on native Windows.
    auto stream = std::ifstream{std::filesystem::path{AOBUS_WINDOWS_WINUI_CMAKE}, std::ios::binary};
    REQUIRE(stream.is_open());
    auto const project = std::string{std::istreambuf_iterator{stream}, std::istreambuf_iterator<char>{}};

    // MSBuild spells the packaged path with escaped backslashes; the shared
    // constant spells it with forward ones.
    auto folder = std::string{kShellPresetFolder};
    auto const separator = folder.find('/');
    REQUIRE(separator != std::string::npos);
    folder.replace(separator, 1, "\\\\");

    CHECK(shellPresetId(ShellPreset::Modern) == "windows.modern");
    CHECK(shellPresetId(ShellPreset::Classic) == "windows.classic");
    CHECK(shellPresetResource(ShellPreset::Modern) == "windows_modern_layout.yaml");
    CHECK(shellPresetResource(ShellPreset::Classic) == "windows_classic_layout.yaml");

    for (auto const preset : {ShellPreset::Modern, ShellPreset::Classic})
    {
      auto const resource = shellPresetResource(preset);
      INFO("preset " << resource);
      CHECK(project.contains(std::format("layout/{}", resource)));
      CHECK(project.contains(std::format("Link={}\\\\{}\"", folder, resource)));
    }
  }

  TEST_CASE("prepareShellPresetDocument - both shipped presets resolve frame commands to offered registrations",
            "[winui][unit][layout]")
  {
    auto const commands = layout::ShellCommands{
      .openLibrary = [] {},
      .rescanLibrary = [] {},
      .toggleInspector = [] {},
      .revealCurrentTrack = [] {},
      .presentTrackProperties = [] {},
      .showSoul = [] {},
      .showSystemMenu = [] {},
    };
    auto offeredActions = std::set<std::string, std::less<>>{};
    layout::registerShellCommandActions(commands,
                                        [&offeredActions](std::string_view const id, std::function<void()> /*command*/)
                                        { REQUIRE(offeredActions.emplace(id).second); });
    REQUIRE(offeredActions.size() == 7);

    // Only non-extracted native registrations retain lexical guards. These
    // source checks do not activate playback or forward a selector anchor.
    auto const builder = readShellBuilderSource();
    auto const schema = layoutSchema();

    for (auto const preset : {ShellPreset::Modern, ShellPreset::Classic})
    {
      auto const resource = shellPresetResource(preset);
      INFO("preset " << resource);

      auto const preparedRes = prepareShellPresetDocument(readShippedDocument(preset), resource);
      REQUIRE(preparedRes.has_value());

      auto actions = std::vector<std::string>{};
      collectBoundActions(schema, preparedRes->effectiveRoot(), actions);
      REQUIRE_FALSE(actions.empty());

      for (auto const& action : actions)
      {
        INFO("action " << action);

        if (offeredActions.contains(action))
        {
          continue;
        }

        if (action == "playback.showOutputDeviceSelector")
        {
          CHECK(builder.contains("\"playback.showOutputDeviceSelector\""));
          CHECK(builder.contains("showSelector"));
        }
        else if (action.starts_with("playback."))
        {
          CHECK(builder.contains(std::format("\"{}\"", action)));
          CHECK(builder.contains("playbackCommandActionId"));
          CHECK(builder.contains("tryExecute"));
        }
        else
        {
          FAIL("Shipped frame command has no offered registration: " << action);
        }
      }
    }
  }

  TEST_CASE("prepareShellPresetDocument - modern toggle and classic menu name the inspector command",
            "[winui][unit][layout]")
  {
    /*
     * Below the wide tier the inspector is an overlay, which shows nothing
     * until it is asked for, so a shell with no way to ask cannot read track
     * details at any width it does not seat the pane inline. Modern asks from
     * its own document; Classic authors no toggle of its own and reaches the
     * command through the menu bar the frame composes for it.
     */
    auto const preparedRes =
      prepareShellPresetDocument(readShippedDocument(ShellPreset::Modern), shellPresetResource(ShellPreset::Modern));
    REQUIRE(preparedRes.has_value());

    auto actions = std::vector<std::string>{};
    collectBoundActions(layoutSchema(), preparedRes->effectiveRoot(), actions);
    CHECK(contains(actions, "shell.toggleInspector"));

    // Classic reaches this command from composeMenuBar(), not the document.
    // This is a source guard, not evidence of native menu activation.
    auto const builder = readShellBuilderSource();
    CHECK(builder.contains("\"winui_shell_track_details\""));
    CHECK(builder.contains("commands.toggleInspector"));
  }

  TEST_CASE("prepareShellPresetDocument - both shipped presets parse, expand, and validate", "[winui][unit][layout]")
  {
    for (auto const preset : {ShellPreset::Modern, ShellPreset::Classic})
    {
      auto const resource = shellPresetResource(preset);
      INFO("preset " << resource);

      auto const preparedRes = prepareShellPresetDocument(readShippedDocument(preset), resource);

      REQUIRE(preparedRes.has_value());
      CHECK_FALSE(preparedRes->effectiveRoot().children.empty());
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

      auto const preparedRes = prepareShellPresetDocument(readShippedDocument(preset), resource);
      REQUIRE(preparedRes.has_value());

      auto painted = std::vector<ThemeSurface>{};
      collectSurfaces(preparedRes->effectiveRoot(), painted);

      for (auto const surface : expected)
      {
        INFO("surface " << toString(surface));
        CHECK(std::ranges::find(painted, surface) != painted.end());
      }

      // Classic repeats surface on the playback strip/table and classic.tree on navigation/inspector.
      auto exactExpected = expected;

      if (preset == ShellPreset::Classic)
      {
        exactExpected.push_back(ThemeSurface::Surface);
        exactExpected.push_back(ThemeSurface::ClassicTree);
      }

      std::ranges::sort(painted);
      std::ranges::sort(exactExpected);
      CHECK(painted == exactExpected);
    }
  }

  TEST_CASE("prepareShellPresetDocument - each preset carries the components the shell reconciles",
            "[winui][unit][layout]")
  {
    SECTION("modern")
    {
      auto const preparedRes =
        prepareShellPresetDocument(readShippedDocument(ShellPreset::Modern), shellPresetResource(ShellPreset::Modern));
      REQUIRE(preparedRes.has_value());

      auto const ids = nodeIds(*preparedRes);
      CHECK(contains(ids, "modern-track-table"));
      CHECK(contains(ids, "modern-track-detail"));
      // The inspector shows the selection's artwork above its fields, which is
      // a component of its own rather than part of the detail region.
      CHECK(contains(ids, "modern-inspector-cover"));
      CHECK(contains(ids, "modern-navigation"));
      CHECK(contains(ids, "modern-inspector"));
      CHECK(contains(ids, "modern-title-bar"));

      for (auto const& [id, type] : std::to_array<std::pair<std::string_view, std::string_view>>({
             {"modern-track-table", "track.table"},
             {"modern-track-detail", "track.detail"},
             {"modern-inspector-cover", "track.coverArt"},
             {"modern-navigation", "windows.navigationPane"},
             {"modern-inspector", "windows.inspectorPane"},
             {"modern-title-bar", "windows.titleBar"},
           }))
      {
        INFO("role " << id);
        auto const* node = findById(preparedRes->effectiveRoot(), id);
        REQUIRE(node != nullptr);
        CHECK(node->type == type);
        auto found = std::vector<LayoutNode const*>{};
        collectByType(preparedRes->effectiveRoot(), type, found);
        CHECK(found.size() == 1);
      }
    }

    SECTION("classic")
    {
      auto const preparedRes = prepareShellPresetDocument(
        readShippedDocument(ShellPreset::Classic), shellPresetResource(ShellPreset::Classic));
      REQUIRE(preparedRes.has_value());

      auto const ids = nodeIds(*preparedRes);
      CHECK(contains(ids, "classic-track-table"));
      CHECK(contains(ids, "classic-track-detail"));
      // Classic shows the selection's artwork above its property rows too; the
      // shells differ in how much room they give it, not in whether it is there.
      CHECK(contains(ids, "classic-inspector-cover"));
      CHECK(contains(ids, "classic-navigation"));
      CHECK(contains(ids, "classic-inspector"));
      CHECK(contains(ids, "classic-menu-bar"));
      CHECK(contains(ids, "classic-status-bar"));

      for (auto const& [id, type] : std::to_array<std::pair<std::string_view, std::string_view>>({
             {"classic-track-table", "track.table"},
             {"classic-track-detail", "track.detail"},
             {"classic-inspector-cover", "track.coverArt"},
             {"classic-navigation", "windows.navigationPane"},
             {"classic-inspector", "windows.inspectorPane"},
             {"classic-menu-bar", "app.menuBar"},
             {"classic-status-bar", "windows.statusBar"},
           }))
      {
        INFO("role " << id);
        auto const* node = findById(preparedRes->effectiveRoot(), id);
        REQUIRE(node != nullptr);
        CHECK(node->type == type);
        auto found = std::vector<LayoutNode const*>{};
        collectByType(preparedRes->effectiveRoot(), type, found);
        CHECK(found.size() == 1);
      }
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

      auto const preparedRes = prepareShellPresetDocument(readShippedDocument(preset), resource);
      REQUIRE(preparedRes.has_value());

      auto panes = std::vector<std::pair<LayoutNode const*, std::string>>{};
      collectSelfSizedPanes(preparedRes->effectiveRoot(), {}, panes);
      CHECK_FALSE(panes.empty());

      for (auto const& [node, parentType] : panes)
      {
        INFO("pane " << node->id);
        CHECK(parentType != "split");
        CHECK(parentType != "collapsibleSplit");
        CHECK_FALSE(node->layoutOr<bool>("hexpand", false));
        CHECK_FALSE(node->layout.contains("widthRequest"));
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
    auto const modernRes =
      prepareShellPresetDocument(readShippedDocument(ShellPreset::Modern), shellPresetResource(ShellPreset::Modern));
    REQUIRE(modernRes.has_value());

    auto const classicRes =
      prepareShellPresetDocument(readShippedDocument(ShellPreset::Classic), shellPresetResource(ShellPreset::Classic));
    REQUIRE(classicRes.has_value());

    auto readings = std::vector<LayoutNode const*>{};
    collectStatusReadings(modernRes->effectiveRoot(), readings);
    CHECK(readings.size() == 2);
    CHECK(std::ranges::count_if(readings, [](LayoutNode const* node) { return node->type == "status.trackCount"; }) ==
          1);
    CHECK(std::ranges::count_if(
            readings, [](LayoutNode const* node) { return node->type == "status.selectionInfo"; }) == 1);

    for (auto const* const node : readings)
    {
      INFO("node " << node->id);
      CHECK(node->propertyOr<std::string>("variant", {}) == kSummaryVariant);
    }

    readings.clear();
    collectStatusReadings(classicRes->effectiveRoot(), readings);
    REQUIRE(readings.size() == 1);
    CHECK(readings.front()->type == "status.trackCount");

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
    auto const schema = layoutSchema();

    for (auto const preset : {ShellPreset::Modern, ShellPreset::Classic})
    {
      auto const resource = shellPresetResource(preset);
      INFO("preset " << resource);

      auto const preparedRes = prepareShellPresetDocument(readShippedDocument(preset), resource);
      REQUIRE(preparedRes.has_value());

      CHECK(countPlayPauseControls(schema, preparedRes->effectiveRoot()) == 1);

      auto souls = std::vector<LayoutNode const*>{};
      collectByType(preparedRes->effectiveRoot(), "playback.soulButton", souls);
      CHECK_FALSE(souls.empty());
      REQUIRE(souls.size() == 1);
      CHECK(souls.front()->id == (preset == ShellPreset::Modern ? "modern-soul" : "classic-soul"));

      auto const optComponent = schema.component("playback.soulButton");
      REQUIRE(optComponent);

      auto const optPrimary = optComponent->actionId(*souls.front(), uimodel::ActionSlot::PrimaryClick);

      if (preset == ShellPreset::Modern)
      {
        CHECK_FALSE(optPrimary);
      }
      else
      {
        REQUIRE(optPrimary);
        CHECK(*optPrimary == "playback.showOutputDeviceSelector");
      }

      auto transports = std::vector<LayoutNode const*>{};
      collectByType(preparedRes->effectiveRoot(), "playback.transportButton", transports);
      auto const playPauseButtons = std::ranges::count_if(
        transports,
        [](LayoutNode const* node) { return node->propertyOr<std::string>("command", "playPause") == "playPause"; });
      CHECK(playPauseButtons == (preset == ShellPreset::Classic ? 1 : 0));

      for (auto const* const soul : souls)
      {
        INFO("soul " << soul->id);
        // The gestures that compete with nothing the soul does are the shell's
        // everywhere, so no preset has to remember to author them.
        CHECK(optComponent->actionId(*soul, uimodel::ActionSlot::SecondaryClick));
        CHECK(optComponent->actionId(*soul, uimodel::ActionSlot::PrimaryLongPress));
      }
    }
  }

  TEST_CASE("prepareShellPresetDocument - the presets keep disjoint node ids so state never crosses shells",
            "[winui][unit][layout]")
  {
    auto const modernRes =
      prepareShellPresetDocument(readShippedDocument(ShellPreset::Modern), shellPresetResource(ShellPreset::Modern));
    auto const classicRes =
      prepareShellPresetDocument(readShippedDocument(ShellPreset::Classic), shellPresetResource(ShellPreset::Classic));
    REQUIRE(modernRes.has_value());
    REQUIRE(classicRes.has_value());

    CHECK(shellPresetId(ShellPreset::Modern) != shellPresetId(ShellPreset::Classic));

    for (auto const& id : nodeIds(*modernRes))
    {
      INFO("modern id " << id);
      CHECK_FALSE(contains(nodeIds(*classicRes), id));
    }
  }

  TEST_CASE("prepareShellPresetDocument - a defective document is rejected as a whole", "[winui][unit][layout]")
  {
    SECTION("unparsable YAML")
    {
      auto const preparedRes = prepareShellPresetDocument("version: 1\nroot: [unterminated", "broken.yaml");

      REQUIRE_FALSE(preparedRes.has_value());
      CHECK(preparedRes.error().code == Error::Code::FormatRejected);
      CHECK(preparedRes.error().message.contains("broken.yaml"));
    }

    SECTION("an unsupported document version")
    {
      auto const preparedRes = prepareShellPresetDocument("version: 2\nroot:\n  type: box\n", "future.yaml");

      REQUIRE_FALSE(preparedRes.has_value());
      CHECK(preparedRes.error().code == Error::Code::NotSupported);
    }

    SECTION("a component the Windows schema does not register")
    {
      auto const preparedRes = prepareShellPresetDocument("version: 1\nroot:\n  id: root\n  type: tabs\n", "gtk.yaml");

      REQUIRE_FALSE(preparedRes.has_value());
      CHECK(preparedRes.error().code == Error::Code::FormatRejected);
      CHECK(preparedRes.error().message.contains("gtk.yaml"));
      CHECK(preparedRes.error().message.contains("unknown component type"));
    }

    SECTION("a document over the shipped size budget")
    {
      auto const oversized = std::string(uimodel::LayoutDocumentLimits::kDefaultMaxFileBytes + 1, 'x');
      auto const preparedRes = prepareShellPresetDocument(oversized, "huge.yaml");

      REQUIRE_FALSE(preparedRes.has_value());
      CHECK(preparedRes.error().code == Error::Code::ValueTooLarge);
    }
  }
} // namespace ao::winui::test
