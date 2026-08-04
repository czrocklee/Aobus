// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/component/shell/NavigationPane.h"
#include "layout/component/shell/PaneSplitter.h"
#include "layout/runtime/ComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include "layout/runtime/UiSubscription.h"
#include "pch.h"
#include <ao/Error.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/layout/shell/ShellGenerationSequence.h>
#include <ao/utility/Path.h>
#include <ao/winui/DesktopSettingsYamlSchema.h>
#include <ao/winui/layout/ShellStatePolicy.h>

#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Windows.Foundation.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace ao::winui::layout
{
  namespace
  {
    using winrt::Microsoft::UI::Xaml::CornerRadius;
    using winrt::Microsoft::UI::Xaml::FrameworkElement;
    using winrt::Microsoft::UI::Xaml::HorizontalAlignment;
    using winrt::Microsoft::UI::Xaml::SizeChangedEventArgs;
    using winrt::Microsoft::UI::Xaml::Thickness;
    using winrt::Microsoft::UI::Xaml::Visibility;
    using winrt::Microsoft::UI::Xaml::Controls::Border;
    using winrt::Microsoft::UI::Xaml::Controls::Grid;
    using winrt::Microsoft::UI::Xaml::Controls::MenuBar;
    using winrt::Microsoft::UI::Xaml::Controls::TextBlock;
    using winrt::Microsoft::UI::Xaml::Controls::Primitives::Popup;
    using winrt::Microsoft::UI::Xaml::Media::Brush;
    using winrt::Windows::Foundation::IInspectable;

    /// The library root, shown as shell state rather than as authored text.
    class LibraryPathComponent final : public LayoutComponent
    {
    public:
      explicit LibraryPathComponent(std::filesystem::path const& libraryRoot)
      {
        _text.Text(winrt::to_hstring(utility::pathToUtf8(libraryRoot)));
      }

      FrameworkElement element() const override { return _text; }

    private:
      TextBlock _text{};
    };

    /// The fill the frame gives a pane that has to float over the workspace.
    constexpr auto kInspectorOverlayFillKey = std::string_view{"InspectorOverlayFillBrush"};

    /**
     * @brief The paint a document put on the inspector's slot.
     *
     * Held apart from the element so the pane can hand it to whichever of the
     * two the user is looking at. Neither margin nor alignment belongs here:
     * those place the slot, and the slot stays where the document put it.
     */
    struct PaneChrome final
    {
      Brush background{nullptr};
      Brush borderBrush{nullptr};
      Thickness borderThickness{};
      CornerRadius cornerRadius{};
      Thickness padding{};
    };

    /**
     * @brief The track inspector region and the boundary the user drags.
     *
     * The width lives in `DesktopSettings`, which owns it together with
     * the rest of the window state, so the pane reads and writes it through the
     * build context instead of the generic component-state store. The resolved
     * inspector pane mode decides whether the pane takes a column of its own or
     * floats over the workspace.
     */
    class InspectorPaneComponent final : public LayoutContainer
    {
    public:
      InspectorPaneComponent(LayoutBuildContext& ctx, Brush overlayFill)
        : _overlayFill{std::move(overlayFill)}
        , _settings{ctx.paneSettings}
        , _gatePtr{ctx.gatePtr}
        , _splitter{PaneEdge::Leading, [this](double const change) { resizeBy(change); }, [this] { commitWidth(); }}
        , _shellState{ctx.shellState}
      {
        auto const splitter = _splitter.element();
        splitter.HorizontalAlignment(HorizontalAlignment::Left);

        _content.HorizontalAlignment(HorizontalAlignment::Stretch);
        _root.Children().Append(_content);
        _root.Children().Append(splitter);

        /*
         * Light dismiss would swallow the click on the command that opened the
         * overlay: the shell's request would still read as set while the card
         * shut, so the next click would reopen what the user meant to close.
         * The command that reveals the overlay therefore also closes it. Not
         * taking the pointer has a second use: the table underneath stays live,
         * which is what lets the floating details follow the selection.
         */
        _overlay.IsLightDismissEnabled(false);
        _root.Children().Append(_overlay);

        _sizeChangedRevoker = _root.SizeChanged(
          winrt::auto_revoke, [this](IInspectable const&, SizeChangedEventArgs const&) { applyOverlayBounds(); });
        applyPaneState();

        // Common authored properties are applied after this constructor. Wait
        // until the element is attached before a floating pane captures that
        // chrome, while retaining the latest state delivered in the meantime.
        _loadedRevoker =
          _root.Loaded(winrt::auto_revoke,
                       [this](IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
                       {
                         _loaded = true;
                         applyUiUpdate("InspectorPaneComponent", [this] { applyShellState(_shellState); });
                       });
        _shellStateSub = subscribeUiUpdate(
          ctx.shellStateChanged, "InspectorPaneComponent", [this](ShellState const state) { setShellState(state); });
      }

      InspectorPaneComponent(InspectorPaneComponent const&) = delete;
      InspectorPaneComponent& operator=(InspectorPaneComponent const&) = delete;
      InspectorPaneComponent(InspectorPaneComponent&&) = delete;
      InspectorPaneComponent& operator=(InspectorPaneComponent&&) = delete;

      /**
       * @brief Take the floating card down with the generation.
       *
       * A popup's content is hosted by the window rather than by the slot it is
       * anchored to, so a generation that is retired while the overlay is open
       * would leave the card floating over the one that replaces it.
       */
      ~InspectorPaneComponent() override
      {
        _overlay.IsOpen(false);
        _overlay.Child(nullptr);
      }

      FrameworkElement element() const override { return _root; }

      void adopt(std::vector<PlacedChild> children) override
      {
        if (!children.empty())
        {
          _content.Child(children.front().componentPtr->element());
        }

        _children = std::move(children);
      }

    private:
      void setShellState(ShellState const& state)
      {
        _shellState = state;

        if (_loaded)
        {
          applyShellState(state);
        }
      }

      void applyShellState(ShellState const& state)
      {
        _inlinePane = state.inspector == InspectorPaneMode::Inline;
        _revealed = state.inspectorRevealed;
        _root.HorizontalAlignment(_inlinePane ? HorizontalAlignment::Stretch : HorizontalAlignment::Right);
        applyPaneState();
      }

      /**
       * @brief Show the pane where the resolved state puts it, or not at all.
       *
       * Inline the pane is a column of the workspace. Revealed as an overlay it
       * must neither take width from the workspace nor be clipped by it, which
       * only a popup guarantees, so the body moves into the popup and the slot
       * stays behind to anchor it. Unrevealed - which either pane mode can be,
       * because the user may also dismiss an inline pane - the slot
       * collapses and the container gives the space back.
       */
      void applyPaneState()
      {
        auto const floating = !_inlinePane && _revealed;
        _root.Visibility(_revealed ? Visibility::Visible : Visibility::Collapsed);
        _splitter.setVisible(_inlinePane);
        floatContent(floating);
        applyWidth();
      }

      /**
       * @brief Move the pane body between its inline slot and the floating card.
       *
       * The chrome the document authored belongs to whichever of the two the
       * user is looking at, so it travels with the body: a slot that kept it
       * would draw its own border as a sliver at the workspace edge once it is
       * narrowed to nothing.
       *
       * The card is that chrome over the frame's backdrop rather than instead
       * of it. A pane's surface is a tint meant to sit on the window and may be
       * translucent on purpose, which inline is the point and floating is the
       * track table reading through the fields; keeping both layers preserves
       * what the theme asked for and still stops the rows.
       *
       * The chrome is read the first time it is needed rather than at
       * construction, because a document's style and themed surface reach the
       * slot only after the component that built it returns.
       */
      void floatContent(bool const floating)
      {
        if (floating == _floating)
        {
          return;
        }

        _floating = floating;

        if (!_optChrome)
        {
          _optChrome = PaneChrome{
            .background = _root.Background(),
            .borderBrush = _root.BorderBrush(),
            .borderThickness = _root.BorderThickness(),
            .cornerRadius = _root.CornerRadius(),
            .padding = _root.Padding(),
          };
        }

        auto const& chrome = *_optChrome;

        if (floating)
        {
          if (std::uint32_t index = 0; _root.Children().IndexOf(_content, index))
          {
            _root.Children().RemoveAt(index);
          }

          _content.Background(chrome.background);
          _content.BorderBrush(chrome.borderBrush);
          _content.BorderThickness(chrome.borderThickness);
          _content.CornerRadius(chrome.cornerRadius);
          _content.Padding(chrome.padding);
          _backdrop.Background(_overlayFill);
          _backdrop.CornerRadius(chrome.cornerRadius);
          _backdrop.Child(_content);
          _root.Background(nullptr);
          _root.BorderThickness(Thickness{});
          _overlay.Child(_backdrop);
          return;
        }

        _overlay.IsOpen(false);
        _overlay.Child(nullptr);
        _backdrop.Child(nullptr);
        _backdrop.ClearValue(FrameworkElement::WidthProperty());
        _backdrop.ClearValue(FrameworkElement::HeightProperty());
        _content.ClearValue(Border::BackgroundProperty());
        _content.ClearValue(Border::BorderBrushProperty());
        _content.ClearValue(Border::BorderThicknessProperty());
        _content.ClearValue(Border::CornerRadiusProperty());
        _content.ClearValue(Border::PaddingProperty());
        _root.Background(chrome.background);
        _root.BorderThickness(chrome.borderThickness);
        _root.Children().InsertAt(0, _content);
      }

      /// The persisted width, or the one a session that has none starts from.
      double paneWidth() const
      {
        return _settings.inspectorWidth ? _settings.inspectorWidth() : kDefaultInspectorPaneWidth;
      }

      /**
       * @brief Give the pane its width inline, and no footprint at all otherwise.
       *
       * An overlay inspector must not take space from the workspace, and the
       * slot its parent allocated follows the element's own size, so the pane
       * keeps the slot only as the anchor the popup is placed against.
       */
      void applyWidth()
      {
        _root.Width(_inlinePane ? paneWidth() : 0.0);
        applyOverlayBounds();
      }

      /**
       * @brief Size and place the floating card over the workspace.
       *
       * The popup is anchored to the pane's own slot, which the document has
       * already placed at the workspace's inspector edge and inset by whatever
       * margin the authored chrome carries. Stepping back its own width is
       * therefore all the card has to do to cover the workspace instead of
       * hanging off it, and it lands exactly where the inline pane would.
       *
       * A card is opened only once the slot has been measured: an overlay with
       * no height to fill would flash at its anchor before the first layout
       * pass reaches it.
       */
      void applyOverlayBounds()
      {
        if (!_floating)
        {
          return;
        }

        auto const width = paneWidth();
        auto const height = _root.ActualHeight();
        _backdrop.Width(width);
        _backdrop.Height(height);
        _overlay.HorizontalOffset(-width);
        _overlay.VerticalOffset(0.0);
        _overlay.IsOpen(height > 0.0);
      }

      void resizeBy(double const change)
      {
        if (!_settings.inspectorWidth || !_settings.setInspectorWidth || !uimodel::isGenerationActive(_gatePtr))
        {
          return;
        }

        _settings.setInspectorWidth(
          std::clamp(_settings.inspectorWidth() + change, kMinimumInspectorPaneWidth, kMaximumInspectorPaneWidth));
        applyWidth();
      }

      void commitWidth() const
      {
        if (_settings.commit && uimodel::isGenerationActive(_gatePtr))
        {
          _settings.commit();
        }
      }

      Grid _root{};
      Border _content{};
      Popup _overlay{};
      /// What the floating card is opaque with, under whatever the pane's own surface is.
      Border _backdrop{};
      Brush _overlayFill;
      PaneSettingsAccess _settings;
      std::weak_ptr<uimodel::ShellGenerationGate> _gatePtr;
      PaneSplitter _splitter;
      std::vector<PlacedChild> _children;
      /// Inline and showing until the first resolved state says otherwise, matching the authored slot.
      bool _inlinePane = true;
      bool _revealed = true;
      bool _floating = false;
      bool _loaded = false;
      std::optional<PaneChrome> _optChrome;
      ShellState _shellState;
      FrameworkElement::SizeChanged_revoker _sizeChangedRevoker{};
      FrameworkElement::Loaded_revoker _loadedRevoker{};
      async::Subscription _shellStateSub;
    };

    /// The Classic application menu bar, filled by the shell that owns its commands.
    class MenuBarComponent final : public LayoutComponent
    {
    public:
      explicit MenuBarComponent(MenuBar bar)
        : _bar{std::move(bar)}
      {
      }

      FrameworkElement element() const override { return _bar; }

    private:
      MenuBar _bar;
    };
  } // namespace

  void registerShellComponents(ComponentRegistry& registry)
  {
    registry.registerComponent(
      "windows.libraryPath",
      [](LayoutBuildContext& ctx, uimodel::LayoutNode const& /*node*/) -> Result<std::unique_ptr<LayoutComponent>>
      { return std::make_unique<LibraryPathComponent>(ctx.library.libraryRoot); });

    registry.registerComponent("windows.navigationPane",
                               [](LayoutBuildContext& ctx, uimodel::LayoutNode const& node)
                               { return makeNavigationPane(ctx, node); });

    registry.registerComponent(
      "windows.inspectorPane",
      [](LayoutBuildContext& ctx, uimodel::LayoutNode const& node) -> Result<std::unique_ptr<LayoutComponent>>
      {
        // Every width below the widest presents this pane as an overlay, and a
        // pane's own surface may be a translucent tint, so without the frame's
        // backdrop there is no readable inspector at those widths at all.
        // Refused here rather than discovered by a user at a narrow window.
        auto const boxedKey = winrt::box_value(winrt::to_hstring(kInspectorOverlayFillKey));
        auto const overlayFill =
          ctx.resources && ctx.resources.HasKey(boxedKey) ? ctx.resources.Lookup(boxedKey).try_as<Brush>() : nullptr;

        if (!overlayFill)
        {
          return makeError(Error::Code::NotFound,
                           std::format("Node '{}' needs the window resource '{}', which the frame does not declare",
                                       node.id,
                                       kInspectorOverlayFillKey));
        }

        return std::make_unique<InspectorPaneComponent>(ctx, overlayFill);
      });

    registry.registerComponent(
      "windows.menuBar",
      [](LayoutBuildContext& ctx, uimodel::LayoutNode const& node) -> Result<std::unique_ptr<LayoutComponent>>
      {
        if (!ctx.menus.composeMenuBar)
        {
          return makeError(
            Error::Code::NotFound,
            std::format("Node '{}' shows the application menu bar, which the shell does not offer", node.id));
        }

        auto bar = MenuBar{};
        ctx.menus.composeMenuBar(bar);
        return std::make_unique<MenuBarComponent>(std::move(bar));
      });
  }
} // namespace ao::winui::layout
