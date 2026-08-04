// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/winui/layout/LayoutCatalog.h>

#include <ao/uimodel/layout/action/LayoutActionCapabilities.h>
#include <ao/uimodel/layout/action/LayoutActionCatalog.h>
#include <ao/uimodel/layout/action/LayoutActionSlot.h>
#include <ao/uimodel/layout/component/LayoutComponentActionPolicy.h>
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/presentation/CoverArtPlaceholder.h>
#include <ao/winui/layout/ElementKind.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::winui
{
  using uimodel::CoverArtPlaceholderSlot;
  using uimodel::coverArtPlaceholderStyleId;
  using uimodel::coverArtPlaceholderStyleIds;
  using uimodel::defaultCoverArtPlaceholderStyle;
  using uimodel::kAllExternalActions;
  using uimodel::LayoutActionCatalog;
  using uimodel::LayoutActionSlot;
  using uimodel::LayoutComponentCatalog;
  using uimodel::LayoutComponentCategory;
  using uimodel::LayoutComponentDescriptor;
  using uimodel::LayoutNode;
  using uimodel::LayoutPropertyDescriptor;
  using uimodel::LayoutPropertyKind;
  using uimodel::LayoutValue;

  namespace
  {
    constexpr auto kPresentationProp = std::string_view{"presentation"};
    constexpr auto kNavigationViewPresentation = std::string_view{"navigationView"};
    constexpr auto kTreePresentation = std::string_view{"tree"};
    constexpr auto kFlyoutPresentation = std::string_view{"flyout"};
    constexpr auto kInlinePresentation = std::string_view{"inline"};

    constexpr auto kNavigationPaneType = std::string_view{"windows.navigationPane"};
    constexpr auto kVolumeControlType = std::string_view{"playback.volumeControl"};
    constexpr auto kCoverArtType = std::string_view{"track.coverArt"};
    constexpr auto kDefaultSplitPosition = 0.5;

    LayoutPropertyDescriptor enumProp(std::string_view const name,
                                      std::string_view const label,
                                      std::vector<std::string> values,
                                      std::string_view const defaultValue)
    {
      return {.name = std::string{name},
              .kind = LayoutPropertyKind::Enum,
              .label = std::string{label},
              .defaultValue = LayoutValue{std::string{defaultValue}},
              .enumValues = std::move(values)};
    }

    /// Whether a status reading is a status bar's content or part of a browser summary.
    LayoutPropertyDescriptor summaryVariantProp()
    {
      return enumProp(
        "variant", "Variant", {std::string{kStatusVariant}, std::string{kSummaryVariant}}, kStatusVariant);
    }

    LayoutPropertyDescriptor orientationProp(std::string_view const defaultValue)
    {
      return enumProp("orientation", "Orientation", {"horizontal", "vertical"}, defaultValue);
    }

    LayoutPropertyDescriptor scalarProp(std::string_view const name,
                                        std::string_view const label,
                                        LayoutPropertyKind const kind,
                                        LayoutValue defaultValue)
    {
      return {
        .name = std::string{name}, .kind = kind, .label = std::string{label}, .defaultValue = std::move(defaultValue)};
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

    void registerDescriptor(LayoutComponentCatalog& catalog, LayoutComponentDescriptor descriptor)
    {
      catalog.registerComponentDescriptor(componentDescriptorWithActionProperties(std::move(descriptor)));
    }

    void registerContainers(LayoutComponentCatalog& catalog)
    {
      registerDescriptor(catalog,
                         {.type = "box",
                          .displayName = "Box",
                          .category = LayoutComponentCategory::Container,
                          .props = {orientationProp("vertical"),
                                    scalarProp("spacing", "Spacing", LayoutPropertyKind::Int, LayoutValue{0L})}});

      registerDescriptor(catalog,
                         {.type = "split",
                          .displayName = "Split",
                          .category = LayoutComponentCategory::Container,
                          .props = {orientationProp("horizontal"),
                                    scalarProp("initialPositionPercent",
                                               "Initial Position",
                                               LayoutPropertyKind::Double,
                                               LayoutValue{kDefaultSplitPosition})},
                          .minChildren = 2,
                          .optMaxChildren = 2});
    }

    void registerShellComponents(LayoutComponentCatalog& catalog)
    {
      registerDescriptor(
        catalog,
        {.type = "windows.titleBar", .displayName = "Title Bar", .category = LayoutComponentCategory::Application});

      registerDescriptor(catalog,
                         {.type = std::string{kNavigationPaneType},
                          .displayName = "Navigation Pane",
                          .category = LayoutComponentCategory::Library,
                          .props = {enumProp(kPresentationProp,
                                             "Presentation",
                                             {std::string{kNavigationViewPresentation}, std::string{kTreePresentation}},
                                             kNavigationViewPresentation)},
                          .optMaxChildren = 1});

      registerDescriptor(catalog,
                         {.type = "windows.inspectorPane",
                          .displayName = "Inspector Pane",
                          .category = LayoutComponentCategory::Application,
                          .minChildren = 1,
                          .optMaxChildren = 1});

      registerDescriptor(catalog,
                         {.type = "windows.libraryPath",
                          .displayName = "Library Path",
                          .category = LayoutComponentCategory::Library,
                          .optMaxChildren = 0});

      registerDescriptor(catalog,
                         {.type = "windows.menuBar",
                          .displayName = "Menu Bar",
                          .category = LayoutComponentCategory::Application,
                          .optMaxChildren = 0});

      registerDescriptor(
        catalog,
        {.type = "windows.statusBar", .displayName = "Status Bar", .category = LayoutComponentCategory::Application});
    }

    void registerTrackComponents(LayoutComponentCatalog& catalog)
    {
      registerDescriptor(catalog,
                         {.type = "track.table",
                          .displayName = "Track Table",
                          .category = LayoutComponentCategory::Track,
                          .optMaxChildren = 0});

      registerDescriptor(catalog,
                         {.type = "track.quickFilter",
                          .displayName = "Quick Filter",
                          .category = LayoutComponentCategory::Track,
                          .optMaxChildren = 0});

      registerDescriptor(catalog,
                         {.type = "track.presentationButton",
                          .displayName = "Presentation Button",
                          .category = LayoutComponentCategory::Track,
                          .props = {enumProp("variant", "Variant", {"title", "compact"}, "title")},
                          .optMaxChildren = 0});

      registerDescriptor(catalog,
                         {.type = "track.detail",
                          .displayName = "Track Detail",
                          .category = LayoutComponentCategory::Track,
                          .optMaxChildren = 0});

      // The same type the GTK catalog registers, and for the same subject: the
      // cover of whatever the focused view has selected. How large the slot is
      // belongs to the frame, so the only thing left to author is what to draw
      // when the selection carries no artwork.
      registerDescriptor(catalog,
                         {.type = std::string{kCoverArtType},
                          .displayName = "Cover Art",
                          .category = LayoutComponentCategory::Track,
                          .props = {enumProp("placeholderStyle",
                                             "Placeholder Style",
                                             coverArtPlaceholderStyleIds(),
                                             coverArtPlaceholderStyleId(
                                               defaultCoverArtPlaceholderStyle(CoverArtPlaceholderSlot::Inspector)))},
                          .optMaxChildren = 0});
    }

    void registerPlaybackComponents(LayoutComponentCatalog& catalog)
    {
      registerDescriptor(
        catalog,
        {.type = "playback.transportButton",
         .displayName = "Transport Button",
         .category = LayoutComponentCategory::Playback,
         .props = {enumProp("command",
                            "Command",
                            {"playPause", "play", "pause", "stop", "next", "previous", "shuffle", "repeat"},
                            "playPause")},
         .optMaxChildren = 0});

      registerDescriptor(
        catalog,
        {.type = "playback.soulButton",
         .displayName = "Soul Button",
         .category = LayoutComponentCategory::Playback,
         .props = {scalarProp("strokeWidth", "Stroke Width", LayoutPropertyKind::Double, LayoutValue{}),
                   scalarProp("glyphScale", "Glyph Scale", LayoutPropertyKind::Double, LayoutValue{}),
                   scalarProp("showGlyph", "Show Glyph", LayoutPropertyKind::Bool, LayoutValue{true})},
         .optMaxChildren = 0,
         // The primary click is deliberately left undefaulted: it is the soul's
         // own play/pause gesture, and only a document that names an action for
         // that slot takes it away. The other two gestures compete with nothing
         // the component does, so every shell gets them.
         .actionPolicy = {.slotMask = kAllExternalActions.slotMask,
                          .defaultActionIds = {{LayoutActionSlot::SecondaryClick, "shell.showSystemMenu"},
                                               {LayoutActionSlot::PrimaryLongPress, "shell.showSoul"}}}});

      registerDescriptor(
        catalog,
        {.type = "playback.seekSlider",
         .displayName = "Seek Slider",
         .category = LayoutComponentCategory::Playback,
         .props = {enumProp(
           kPresentationProp, "Presentation", {"overlay", std::string{kInlinePresentation}}, kInlinePresentation)},
         .optMaxChildren = 0});

      registerDescriptor(catalog,
                         {.type = "playback.timeLabel",
                          .displayName = "Time Label",
                          .category = LayoutComponentCategory::Playback,
                          .props = {enumProp("variant", "Variant", {"elapsed", "duration", "combined"}, "elapsed")},
                          .optMaxChildren = 0});

      registerDescriptor(catalog,
                         {.type = std::string{kVolumeControlType},
                          .displayName = "Volume Control",
                          .category = LayoutComponentCategory::Playback,
                          .props = {enumProp(kPresentationProp,
                                             "Presentation",
                                             {std::string{kFlyoutPresentation}, std::string{kInlinePresentation}},
                                             kFlyoutPresentation)},
                          .optMaxChildren = 0});

      registerDescriptor(catalog,
                         {.type = "playback.outputDeviceButton",
                          .displayName = "Output Device Button",
                          .category = LayoutComponentCategory::Playback,
                          .optMaxChildren = 0});

      registerDescriptor(catalog,
                         {.type = "playback.nowPlayingInfo",
                          .displayName = "Now Playing Info",
                          .category = LayoutComponentCategory::Playback,
                          .optMaxChildren = 0});
    }

    void registerStatusComponents(LayoutComponentCatalog& catalog)
    {
      registerDescriptor(catalog,
                         {.type = "status.activity",
                          .displayName = "Activity Status",
                          .category = LayoutComponentCategory::Status,
                          .optMaxChildren = 0});

      // A count or a selection summary reads the same either way; where it sits
      // is what decides whether a narrow window may drop it. In a status bar it
      // is the bar's content and always shows; as part of a browser summary it
      // yields its space to the filter below the wide tier.
      registerDescriptor(catalog,
                         {.type = "status.trackCount",
                          .displayName = "Track Count",
                          .category = LayoutComponentCategory::Status,
                          .props = {summaryVariantProp()},
                          .optMaxChildren = 0});

      registerDescriptor(catalog,
                         {.type = "status.selectionInfo",
                          .displayName = "Selection Info",
                          .category = LayoutComponentCategory::Status,
                          .props = {summaryVariantProp()},
                          .optMaxChildren = 0});

      registerDescriptor(catalog,
                         {.type = "status.message",
                          .displayName = "Status Message",
                          .category = LayoutComponentCategory::Status,
                          .optMaxChildren = 0});
    }

    void registerGenericComponents(LayoutComponentCatalog& catalog)
    {
      registerDescriptor(
        catalog,
        {.type = "label",
         .displayName = "Label",
         .category = LayoutComponentCategory::Generic,
         .props = {scalarProp("resourceKey", "Resource Key", LayoutPropertyKind::String, LayoutValue{std::string{}})},
         .optMaxChildren = 0});

      registerDescriptor(
        catalog,
        {.type = "actionButton",
         .displayName = "Action Button",
         .category = LayoutComponentCategory::Generic,
         .props = {scalarProp("resourceKey", "Resource Key", LayoutPropertyKind::String, LayoutValue{std::string{}}),
                   scalarProp("glyph", "Glyph", LayoutPropertyKind::String, LayoutValue{std::string{}})},
         .optMaxChildren = 0,
         .actionPolicy = kAllExternalActions});

      registerDescriptor(
        catalog,
        {.type = "menuButton",
         .displayName = "Menu Button",
         .category = LayoutComponentCategory::Generic,
         .props = {enumProp("menuId", "Menu", {"modernOverflow", "nowPlayingOverflow"}, "modernOverflow"),
                   scalarProp("resourceKey", "Resource Key", LayoutPropertyKind::String, LayoutValue{std::string{}}),
                   scalarProp("glyph", "Glyph", LayoutPropertyKind::String, LayoutValue{std::string{}})},
         .optMaxChildren = 0});
    }

    void registerAction(LayoutActionCatalog& catalog,
                        std::string_view const id,
                        std::string_view const label,
                        std::string_view const category,
                        uimodel::LayoutActionCapabilities const capabilities = uimodel::LayoutActionCapability::None)
    {
      catalog.registerActionDescriptor({.id = std::string{id},
                                        .label = std::string{label},
                                        .category = std::string{category},
                                        .capabilities = capabilities});
    }
  } // namespace

  LayoutComponentCatalog layoutCatalog()
  {
    auto catalog = LayoutComponentCatalog{};
    registerContainers(catalog);
    registerShellComponents(catalog);
    registerTrackComponents(catalog);
    registerPlaybackComponents(catalog);
    registerStatusComponents(catalog);
    registerGenericComponents(catalog);
    return catalog;
  }

  LayoutActionCatalog layoutActionCatalog()
  {
    // Menus, transport commands, and column editing are native behavior of the
    // component that owns them, so only the ids the two presets actually bind
    // are registered here.
    auto catalog = LayoutActionCatalog{};
    registerAction(catalog, "library.open", "Open Library", "Library", uimodel::LayoutActionCapability::RequiresAnchor);
    registerAction(catalog, "library.rescan", "Rescan Library", "Library");
    registerAction(catalog, "shell.toggleInspector", "Toggle Inspector", "Shell");
    registerAction(catalog,
                   "shell.showSystemMenu",
                   "System Menu",
                   "Shell",
                   uimodel::LayoutActionCapability::RequiresAnchor | uimodel::LayoutActionCapability::PresentsMenu);
    registerAction(catalog, "shell.showSoul", "Show Soul", "Shell");
    registerAction(catalog,
                   "playback.showOutputDeviceSelector",
                   "Output Device",
                   "Playback",
                   uimodel::LayoutActionCapability::RequiresAnchor | uimodel::LayoutActionCapability::PresentsMenu);
    return catalog;
  }

  std::optional<ElementKind> componentElementKind(LayoutNode const& node)
  {
    if (node.type == kNavigationPaneType)
    {
      // The tree presentation is a Grid rather than the TreeView it shows: like
      // the inspector, the pane carries a resize thumb over its trailing edge,
      // so the thumb and the tree share a cell. A NavigationView draws its own
      // pane and needs no such cell.
      return presentationOf(node, kNavigationViewPresentation) == kTreePresentation ? ElementKind::Grid
                                                                                    : ElementKind::NavigationView;
    }

    if (node.type == kVolumeControlType)
    {
      return presentationOf(node, kFlyoutPresentation) == kInlinePresentation ? ElementKind::Slider
                                                                              : ElementKind::Button;
    }

    // Every structural container is a Grid: WinUI expresses "take the remaining
    // space" on a row or column definition, which is what `hexpand`/`vexpand`
    // mean, and no stacking panel can allocate that slot.
    if (node.type == "box" || node.type == "split" || node.type == "windows.titleBar" ||
        node.type == "windows.statusBar" || node.type == "playback.nowPlayingInfo" || node.type == "status.activity")
    {
      return ElementKind::Grid;
    }

    // Both scroll their own content and are the viewport, not the surface
    // inside it: the detail scrolls vertically, and the table horizontally over
    // a surface as wide as the solved columns make it.
    if (node.type == "track.detail" || node.type == "track.table")
    {
      return ElementKind::ScrollViewer;
    }

    if (node.type == "windows.inspectorPane")
    {
      // The pane is a Grid, not the Border its chrome suggests: the resize
      // thumb overlays the pane's leading edge, so the two share a cell.
      return ElementKind::Grid;
    }

    if (node.type == kCoverArtType)
    {
      // A Border rather than the Image it shows: artwork and placeholder are
      // layered behind one rounded corner radius, and only a Border clips its
      // child to that radius.
      return ElementKind::Border;
    }

    if (node.type == "windows.menuBar")
    {
      return ElementKind::MenuBar;
    }

    if (node.type == "track.quickFilter")
    {
      return ElementKind::AutoSuggestBox;
    }

    if (node.type == "track.presentationButton" || node.type == "playback.transportButton" ||
        node.type == "playback.soulButton" || node.type == "playback.outputDeviceButton" ||
        node.type == "actionButton" || node.type == "menuButton")
    {
      return ElementKind::Button;
    }

    if (node.type == "playback.seekSlider")
    {
      return ElementKind::Slider;
    }

    if (node.type == "playback.timeLabel" || node.type == "status.trackCount" || node.type == "status.selectionInfo" ||
        node.type == "status.message" || node.type == "windows.libraryPath" || node.type == "label")
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

    // The NavigationView presentation owns the workspace as its content region;
    // the tree presentation is a sibling column and hosts nothing.
    return presentationOf(node, kNavigationViewPresentation) == kTreePresentation ? std::size_t{0} : std::size_t{1};
  }
} // namespace ao::winui
