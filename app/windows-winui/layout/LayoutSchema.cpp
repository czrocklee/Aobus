// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/winui/layout/LayoutSchema.h>

#include <ao/Contract.h>
#include <ao/uimodel/layout/component/LayoutSchema.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/playback/command/PlaybackCommand.h>
#include <ao/winui/layout/ElementKind.h>

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::winui
{
  using uimodel::ActionSlot;
  using uimodel::ComponentCategory;
  using uimodel::ComponentSchema;
  using uimodel::ComponentSchemaExtension;
  using uimodel::LayoutNode;
  using uimodel::LayoutSchema;
  using uimodel::LayoutValue;
  using uimodel::PropertyKind;
  using uimodel::PropertySchema;

  namespace
  {
    constexpr auto kPresentationProp = std::string_view{"presentation"};
    constexpr auto kNavigationViewPresentation = std::string_view{"navigationView"};
    constexpr auto kTreePresentation = std::string_view{"tree"};
    constexpr auto kFlyoutPresentation = std::string_view{"flyout"};
    constexpr auto kInlinePresentation = std::string_view{"inline"};
    constexpr auto kNavigationPaneType = std::string_view{"windows.navigationPane"};
    constexpr auto kDefaultSplitPosition = 0.5;

    constexpr auto kWindowsBindableActionSlots = uimodel::actionSlotBit(ActionSlot::PrimaryClick) |
                                                 uimodel::actionSlotBit(ActionSlot::PrimaryLongPress) |
                                                 uimodel::actionSlotBit(ActionSlot::SecondaryClick);

    PropertySchema enumProp(std::string_view const name,
                            std::string_view const label,
                            std::vector<std::string> values,
                            std::string_view const defaultValue)
    {
      return {.name = std::string{name},
              .kind = PropertyKind::Enum,
              .label = std::string{label},
              .defaultValue = LayoutValue{std::string{defaultValue}},
              .enumValues = std::move(values)};
    }

    PropertySchema scalarProp(std::string_view const name,
                              std::string_view const label,
                              PropertyKind const kind,
                              LayoutValue defaultValue)
    {
      return {
        .name = std::string{name}, .kind = kind, .label = std::string{label}, .defaultValue = std::move(defaultValue)};
    }

    PropertySchema summaryVariantProp()
    {
      return enumProp(
        "variant", "Variant", {std::string{kStatusVariant}, std::string{kSummaryVariant}}, kStatusVariant);
    }

    bool isAnyOf(LayoutNode const& node, std::initializer_list<std::string_view> const ids)
    {
      return std::ranges::contains(ids, node.type);
    }

    std::string_view presentationOf(LayoutNode const& node, std::string_view const fallback)
    {
      auto const it = node.props.find(kPresentationProp);

      if (it == node.props.end())
      {
        return fallback;
      }

      auto const* const value = it->second.getIf<std::string>();
      return value == nullptr || value->empty() ? fallback : std::string_view{*value};
    }

    void addComponent(LayoutSchema& schema, ComponentSchema component)
    {
      auto const added = schema.addComponent(std::move(component));
      AO_EXPECTS(added, "WinUI component ids must be unique and component schemas must be valid");
    }

    void addShared(LayoutSchema& schema, std::string_view const id, ComponentSchemaExtension extension = {})
    {
      auto const added = schema.addSharedComponent(id, std::move(extension));
      AO_EXPECTS(added, "WinUI shared components must import one canonical schema entry");
    }

    void addContainers(LayoutSchema& schema)
    {
      addShared(schema, "box");
      addShared(
        schema,
        "split",
        {.properties = {scalarProp(
           "initialPositionPercent", "Initial Position", PropertyKind::Double, LayoutValue{kDefaultSplitPosition})}});
    }

    void addShellComponents(LayoutSchema& schema)
    {
      addComponent(
        schema, {.id = "windows.titleBar", .displayName = "Title Bar", .category = ComponentCategory::Application});
      addComponent(schema,
                   {.id = std::string{kNavigationPaneType},
                    .displayName = "Navigation Pane",
                    .category = ComponentCategory::Library,
                    .properties = {enumProp(kPresentationProp,
                                            "Presentation",
                                            {std::string{kNavigationViewPresentation}, std::string{kTreePresentation}},
                                            kNavigationViewPresentation)},
                    .optMaxChildren = 1});
      addComponent(schema,
                   {.id = "windows.inspectorPane",
                    .displayName = "Inspector Pane",
                    .category = ComponentCategory::Application,
                    .minChildren = 1,
                    .optMaxChildren = 1});
      addComponent(schema,
                   {.id = "windows.libraryPath",
                    .displayName = "Library Path",
                    .category = ComponentCategory::Library,
                    .optMaxChildren = 0});
      addShared(schema, "app.menuBar");
      addComponent(
        schema, {.id = "windows.statusBar", .displayName = "Status Bar", .category = ComponentCategory::Application});
    }

    void addTrackComponents(LayoutSchema& schema)
    {
      addShared(schema, "track.table");
      addShared(schema, "track.quickFilter");
      addShared(schema, "track.presentationButton");
      addComponent(schema,
                   {.id = "track.detail",
                    .displayName = "Track Detail",
                    .category = ComponentCategory::Track,
                    .optMaxChildren = 0});
      addShared(schema, "track.coverArt");
    }

    void addPlaybackComponents(LayoutSchema& schema)
    {
      addShared(schema, "playback.transportButton");
      addShared(schema,
                "playback.soulButton",
                {.properties = {scalarProp("showGlyph", "Show Glyph", PropertyKind::Bool, LayoutValue{true})},
                 .defaultActions = {{.slot = ActionSlot::SecondaryClick, .actionId = "shell.showSystemMenu"},
                                    {.slot = ActionSlot::PrimaryLongPress, .actionId = "shell.showSoul"}}});
      addShared(
        schema,
        "playback.seekSlider",
        {.properties = {enumProp(
           kPresentationProp, "Presentation", {"overlay", std::string{kInlinePresentation}}, kInlinePresentation)}});
      addShared(schema, "playback.timeLabel");
      addShared(schema,
                "playback.volumeControl",
                {.properties = {enumProp(kPresentationProp,
                                         "Presentation",
                                         {std::string{kFlyoutPresentation}, std::string{kInlinePresentation}},
                                         kFlyoutPresentation)}});
      addShared(schema, "playback.outputDeviceSelector");
      addComponent(schema,
                   {.id = "playback.nowPlayingInfo",
                    .displayName = "Now Playing Info",
                    .category = ComponentCategory::Playback,
                    .optMaxChildren = 0});
    }

    void addStatusComponents(LayoutSchema& schema)
    {
      addShared(schema, "status.activity");
      addShared(schema, "status.trackCount", {.properties = {summaryVariantProp()}});
      addShared(schema, "status.selectionInfo", {.properties = {summaryVariantProp()}});
      addShared(schema, "status.message");
    }

    PropertySchema glyphProp()
    {
      return scalarProp("glyph", "Glyph", PropertyKind::String, LayoutValue{std::string{}});
    }

    PropertySchema textResourceKeyProp()
    {
      return scalarProp("textResourceKey", "Text Resource", PropertyKind::String, LayoutValue{std::string{}});
    }

    void addGenericComponents(LayoutSchema& schema)
    {
      addShared(schema, "label", {.properties = {textResourceKeyProp()}});
      addShared(schema,
                "actionButton",
                {.properties = {glyphProp(), textResourceKeyProp()}, .actionSlots = kWindowsBindableActionSlots});
      addShared(schema,
                "menuButton",
                {.properties = {enumProp("menuId", "Menu", {"modernOverflow", "nowPlayingOverflow"}, "modernOverflow"),
                                glyphProp(),
                                textResourceKeyProp()}});
    }

    void addAction(LayoutSchema& schema,
                   std::string_view const id,
                   std::string_view const label,
                   std::string_view const category,
                   uimodel::ActionCapabilityMask const capabilities = 0)
    {
      auto const added = schema.addAction({.id = std::string{id},
                                           .label = std::string{label},
                                           .category = std::string{category},
                                           .capabilities = capabilities});
      AO_EXPECTS(added, "WinUI action ids must be unique");
    }

    void addActions(LayoutSchema& schema)
    {
      for (auto const command : uimodel::playbackCommands())
      {
        addAction(schema,
                  uimodel::playbackCommandActionId(command),
                  uimodel::playbackCommandLabel(command),
                  uimodel::kPlaybackActionCategory);
      }

      addAction(schema,
                "library.open",
                "Open Library",
                "Library",
                uimodel::actionCapabilityBit(uimodel::ActionCapability::RequiresAnchor));
      addAction(schema, "library.rescan", "Rescan Library", "Library");
      addAction(schema, "shell.toggleInspector", "Toggle Inspector", "Shell");
      addAction(schema,
                "shell.showSystemMenu",
                "System Menu",
                "Shell",
                uimodel::ActionCapability::RequiresAnchor | uimodel::ActionCapability::PresentsMenu);
      addAction(schema, "shell.showSoul", "Show Soul", "Shell");
      addAction(schema,
                "playback.showOutputDeviceSelector",
                "Output Device",
                "Playback",
                uimodel::ActionCapability::RequiresAnchor | uimodel::ActionCapability::PresentsMenu);
    }
  } // namespace

  LayoutSchema layoutSchema()
  {
    auto schema = LayoutSchema{};
    addContainers(schema);
    addShellComponents(schema);
    addTrackComponents(schema);
    addPlaybackComponents(schema);
    addStatusComponents(schema);
    addGenericComponents(schema);
    addActions(schema);
    return schema;
  }

  std::optional<ElementKind> componentElementKind(LayoutNode const& node)
  {
    if (node.type == kNavigationPaneType)
    {
      return presentationOf(node, kNavigationViewPresentation) == kTreePresentation ? ElementKind::Grid
                                                                                    : ElementKind::NavigationView;
    }

    if (node.type == "playback.volumeControl")
    {
      return presentationOf(node, kFlyoutPresentation) == kInlinePresentation ? ElementKind::Slider
                                                                              : ElementKind::Button;
    }

    if (isAnyOf(node, {"box", "split", "status.activity"}) || node.type == "windows.titleBar" ||
        node.type == "windows.statusBar" || node.type == "playback.nowPlayingInfo")
    {
      return ElementKind::Grid;
    }

    if (node.type == "track.detail" || node.type == "track.table")
    {
      return ElementKind::ScrollViewer;
    }

    if (node.type == "windows.inspectorPane")
    {
      return ElementKind::Grid;
    }

    if (node.type == "track.coverArt")
    {
      return ElementKind::Border;
    }

    if (node.type == "app.menuBar")
    {
      return ElementKind::MenuBar;
    }

    if (node.type == "track.quickFilter")
    {
      return ElementKind::Grid;
    }

    if (isAnyOf(node,
                {"track.presentationButton",
                 "playback.soulButton",
                 "playback.outputDeviceSelector",
                 "actionButton",
                 "menuButton",
                 "playback.transportButton"}))
    {
      return ElementKind::Button;
    }

    if (node.type == "playback.seekSlider")
    {
      return ElementKind::Slider;
    }

    if (isAnyOf(node, {"playback.timeLabel", "status.trackCount", "status.selectionInfo", "status.message", "label"}) ||
        node.type == "windows.libraryPath")
    {
      return ElementKind::TextBlock;
    }

    return std::nullopt;
  }

  bool componentRequiresId(std::string_view const type) noexcept
  {
    return type == kNavigationPaneType || type == "windows.inspectorPane" || type == "track.table" ||
           type == "track.detail";
  }

  std::optional<std::size_t> presentationChildCount(LayoutNode const& node)
  {
    if (node.type != kNavigationPaneType)
    {
      return std::nullopt;
    }

    return presentationOf(node, kNavigationViewPresentation) == kTreePresentation ? std::size_t{0} : std::size_t{1};
  }
} // namespace ao::winui
