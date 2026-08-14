// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/winui/layout/LayoutCatalog.h>

#include <ao/uimodel/layout/action/LayoutActionCapabilities.h>
#include <ao/uimodel/layout/action/LayoutActionCatalog.h>
#include <ao/uimodel/layout/action/LayoutActionSlot.h>
#include <ao/uimodel/layout/component/LayoutComponentActionPolicy.h>
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
#include <ao/uimodel/layout/component/SharedLayoutComponentType.h>
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
  using uimodel::componentTypeName;
  using uimodel::LayoutActionCatalog;
  using uimodel::LayoutActionSlot;
  using uimodel::LayoutComponentCatalog;
  using uimodel::LayoutComponentCategory;
  using uimodel::LayoutComponentDescriptor;
  using uimodel::LayoutNode;
  using uimodel::LayoutPropertyDescriptor;
  using uimodel::LayoutPropertyKind;
  using uimodel::LayoutValue;
  using uimodel::sharedComponentDescriptor;
  using uimodel::SharedLayoutComponentType;
  using uimodel::withShellProperties;

  namespace
  {
    constexpr auto kPresentationProp = std::string_view{"presentation"};
    constexpr auto kNavigationViewPresentation = std::string_view{"navigationView"};
    constexpr auto kTreePresentation = std::string_view{"tree"};
    constexpr auto kFlyoutPresentation = std::string_view{"flyout"};
    constexpr auto kInlinePresentation = std::string_view{"inline"};

    constexpr auto kNavigationPaneType = std::string_view{"windows.navigationPane"};
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

    LayoutPropertyDescriptor scalarProp(std::string_view const name,
                                        std::string_view const label,
                                        LayoutPropertyKind const kind,
                                        LayoutValue defaultValue)
    {
      return {
        .name = std::string{name}, .kind = kind, .label = std::string{label}, .defaultValue = std::move(defaultValue)};
    }

    /// Whether @p node names one of the shared @p components.
    bool isAnyOf(LayoutNode const& node, std::initializer_list<SharedLayoutComponentType> const components)
    {
      return std::ranges::any_of(
        components, [&node](auto const component) { return node.type == componentTypeName(component); });
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
      registerDescriptor(catalog, sharedComponentDescriptor(SharedLayoutComponentType::Box));

      // Where an unauthored divider rests is a measurement, and WinUI takes it
      // from a proportional column weight rather than a pixel position.
      registerDescriptor(catalog,
                         withShellProperties(sharedComponentDescriptor(SharedLayoutComponentType::Split),
                                             {scalarProp("initialPositionPercent",
                                                         "Initial Position",
                                                         LayoutPropertyKind::Double,
                                                         LayoutValue{kDefaultSplitPosition})}));
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

      registerDescriptor(catalog, sharedComponentDescriptor(SharedLayoutComponentType::MenuBar));

      registerDescriptor(
        catalog,
        {.type = "windows.statusBar", .displayName = "Status Bar", .category = LayoutComponentCategory::Application});
    }

    void registerTrackComponents(LayoutComponentCatalog& catalog)
    {
      registerDescriptor(catalog, sharedComponentDescriptor(SharedLayoutComponentType::TrackTable));
      registerDescriptor(catalog, sharedComponentDescriptor(SharedLayoutComponentType::TrackQuickFilter));
      registerDescriptor(catalog, sharedComponentDescriptor(SharedLayoutComponentType::TrackPresentationButton));

      registerDescriptor(catalog,
                         {.type = "track.detail",
                          .displayName = "Track Detail",
                          .category = LayoutComponentCategory::Track,
                          .optMaxChildren = 0});

      registerDescriptor(catalog, sharedComponentDescriptor(SharedLayoutComponentType::TrackCoverArt));
    }

    void registerPlaybackComponents(LayoutComponentCatalog& catalog)
    {
      registerDescriptor(catalog, sharedComponentDescriptor(SharedLayoutComponentType::PlaybackTransportButton));

      // The primary click is deliberately left undefaulted: it is the soul's
      // own play/pause gesture, and only a document that names an action for
      // that slot takes it away. The other two gestures compete with nothing
      // the component does, so every shell gets them.
      auto soulButton =
        withShellProperties(sharedComponentDescriptor(SharedLayoutComponentType::PlaybackSoulButton),
                            // This shell's inner mark is the live transport icon, so the only
                            // question a document can answer is whether to draw it. GTK's `glyph`
                            // chooses between two static ornaments, which is a different question
                            // and therefore a different property.
                            {scalarProp("showGlyph", "Show Glyph", LayoutPropertyKind::Bool, LayoutValue{true})});
      soulButton.actionPolicy.defaultActionIds = {{LayoutActionSlot::SecondaryClick, "shell.showSystemMenu"},
                                                  {LayoutActionSlot::PrimaryLongPress, "shell.showSoul"}};
      registerDescriptor(catalog, std::move(soulButton));

      registerDescriptor(
        catalog,
        withShellProperties(
          sharedComponentDescriptor(SharedLayoutComponentType::PlaybackSeekSlider),
          {enumProp(
            kPresentationProp, "Presentation", {"overlay", std::string{kInlinePresentation}}, kInlinePresentation)}));

      registerDescriptor(catalog, sharedComponentDescriptor(SharedLayoutComponentType::PlaybackTimeLabel));

      registerDescriptor(
        catalog,
        withShellProperties(sharedComponentDescriptor(SharedLayoutComponentType::PlaybackVolumeControl),
                            {enumProp(kPresentationProp,
                                      "Presentation",
                                      {std::string{kFlyoutPresentation}, std::string{kInlinePresentation}},
                                      kFlyoutPresentation)}));

      registerDescriptor(catalog, sharedComponentDescriptor(SharedLayoutComponentType::PlaybackOutputDeviceSelector));

      registerDescriptor(catalog,
                         {.type = "playback.nowPlayingInfo",
                          .displayName = "Now Playing Info",
                          .category = LayoutComponentCategory::Playback,
                          .optMaxChildren = 0});
    }

    void registerStatusComponents(LayoutComponentCatalog& catalog)
    {
      registerDescriptor(catalog, sharedComponentDescriptor(SharedLayoutComponentType::StatusActivity));

      // A count or a selection summary reads the same either way; where it sits
      // is what decides whether a narrow window may drop it. In a status bar it
      // is the bar's content and always shows; as part of a browser summary it
      // yields its space to the filter below the wide tier. No other shell
      // draws a summary yet, so the placement is Windows' own.
      registerDescriptor(catalog,
                         withShellProperties(sharedComponentDescriptor(SharedLayoutComponentType::StatusTrackCount),
                                             {summaryVariantProp()}));

      registerDescriptor(catalog,
                         withShellProperties(sharedComponentDescriptor(SharedLayoutComponentType::StatusSelectionInfo),
                                             {summaryVariantProp()}));

      registerDescriptor(catalog, sharedComponentDescriptor(SharedLayoutComponentType::StatusMessage));
    }

    /**
     * @brief Every action slot this shell can actually bind.
     *
     * `ActionBinder` rejects a secondary long press outright, because Windows
     * raises one holding sequence per press regardless of which button started
     * it. A catalog that offered the slot anyway would let a document validate
     * and then fail its whole node at build time, which is a worse answer than
     * refusing the property up front.
     *
     * Every widening in this file goes through this, and a test holds the
     * catalog to it, so the two cannot drift apart.
     */
    uimodel::LayoutComponentActionPolicy const kWindowsBindableActions =
      uimodel::kExternalActionsWithoutSecondaryLongPress;

    LayoutPropertyDescriptor glyphProp()
    {
      // A Segoe codepoint, which no other toolkit can resolve.
      return scalarProp("glyph", "Glyph", LayoutPropertyKind::String, LayoutValue{std::string{}});
    }

    /**
     * @brief Names a string in this shell's resource dictionary.
     *
     * The shared `text` property is the words a reader sees, and a document
     * that sets it shows those words in every shell. Localization is not that:
     * `text: AppTitleValue` would read as the words "AppTitleValue" anywhere
     * else, so naming a resource is a separate, Windows-owned property rather
     * than a second meaning for a shared one.
     *
     * When both are authored this one wins, and a key the dictionary does not
     * define falls back to `text`.
     */
    LayoutPropertyDescriptor textResourceKeyProp()
    {
      return scalarProp("textResourceKey", "Text Resource", LayoutPropertyKind::String, LayoutValue{std::string{}});
    }

    void registerGenericComponents(LayoutComponentCatalog& catalog)
    {
      registerDescriptor(
        catalog,
        withShellProperties(sharedComponentDescriptor(SharedLayoutComponentType::Label), {textResourceKeyProp()}));

      registerDescriptor(
        catalog,
        // This shell binds a right-click gesture that GTK's button does not, so
        // it widens the shared primary-only slots rather than accepting them.
        withShellActionSlots(withShellProperties(sharedComponentDescriptor(SharedLayoutComponentType::ActionButton),
                                                 {glyphProp(), textResourceKeyProp()}),
                             kWindowsBindableActions));

      registerDescriptor(
        catalog,
        withShellProperties(sharedComponentDescriptor(SharedLayoutComponentType::MenuButton),
                            {enumProp("menuId", "Menu", {"modernOverflow", "nowPlayingOverflow"}, "modernOverflow"),
                             glyphProp(),
                             textResourceKeyProp()}));
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

    // The transport is named the same way in every shell, so its ids and labels
    // are derived rather than restated here. A document rarely binds them - the
    // transport buttons run their own command - but a keyboard map does.
    for (auto const command : uimodel::playbackCommands())
    {
      registerAction(catalog,
                     uimodel::playbackCommandActionId(command),
                     uimodel::playbackCommandLabel(command),
                     uimodel::kPlaybackActionCategory);
    }

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

    if (node.type == componentTypeName(SharedLayoutComponentType::PlaybackVolumeControl))
    {
      return presentationOf(node, kFlyoutPresentation) == kInlinePresentation ? ElementKind::Slider
                                                                              : ElementKind::Button;
    }

    // Every structural container is a Grid: WinUI expresses "take the remaining
    // space" on a row or column definition, which is what `hexpand`/`vexpand`
    // mean, and no stacking panel can allocate that slot.
    if (isAnyOf(node,
                {SharedLayoutComponentType::Box,
                 SharedLayoutComponentType::Split,
                 SharedLayoutComponentType::StatusActivity}) ||
        node.type == "windows.titleBar" || node.type == "windows.statusBar" || node.type == "playback.nowPlayingInfo")
    {
      return ElementKind::Grid;
    }

    // Both scroll their own content and are the viewport, not the surface
    // inside it: the detail scrolls vertically, and the table horizontally over
    // a surface as wide as the solved columns make it.
    if (node.type == "track.detail" || node.type == componentTypeName(SharedLayoutComponentType::TrackTable))
    {
      return ElementKind::ScrollViewer;
    }

    if (node.type == "windows.inspectorPane")
    {
      // The pane is a Grid, not the Border its chrome suggests: the resize
      // thumb overlays the pane's leading edge, so the two share a cell.
      return ElementKind::Grid;
    }

    if (node.type == componentTypeName(SharedLayoutComponentType::TrackCoverArt))
    {
      // A Border rather than the Image it shows: artwork and placeholder are
      // layered behind one rounded corner radius, and only a Border clips its
      // child to that radius.
      return ElementKind::Border;
    }

    if (node.type == componentTypeName(SharedLayoutComponentType::MenuBar))
    {
      return ElementKind::MenuBar;
    }

    if (node.type == componentTypeName(SharedLayoutComponentType::TrackQuickFilter))
    {
      return ElementKind::AutoSuggestBox;
    }

    if (isAnyOf(node,
                {SharedLayoutComponentType::TrackPresentationButton,
                 SharedLayoutComponentType::PlaybackSoulButton,
                 SharedLayoutComponentType::PlaybackOutputDeviceSelector,
                 SharedLayoutComponentType::ActionButton,
                 SharedLayoutComponentType::MenuButton,
                 SharedLayoutComponentType::PlaybackTransportButton}))
    {
      return ElementKind::Button;
    }

    if (node.type == componentTypeName(SharedLayoutComponentType::PlaybackSeekSlider))
    {
      return ElementKind::Slider;
    }

    if (isAnyOf(node,
                {SharedLayoutComponentType::PlaybackTimeLabel,
                 SharedLayoutComponentType::StatusTrackCount,
                 SharedLayoutComponentType::StatusSelectionInfo,
                 SharedLayoutComponentType::StatusMessage,
                 SharedLayoutComponentType::Label}) ||
        node.type == "windows.libraryPath")
    {
      return ElementKind::TextBlock;
    }

    return std::nullopt;
  }

  bool componentRequiresId(std::string_view const type) noexcept
  {
    return type == kNavigationPaneType || type == "windows.inspectorPane" ||
           type == componentTypeName(SharedLayoutComponentType::TrackTable) || type == "track.detail";
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
