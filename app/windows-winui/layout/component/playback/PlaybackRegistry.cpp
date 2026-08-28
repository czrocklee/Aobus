// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/component/playback/NowPlayingInfo.h"
#include "layout/component/playback/SoulButton.h"
#include "layout/runtime/ComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include "layout/runtime/ResourceLookup.h"
#include "pch.h"
#include "platform/StringResources.h"
#include "playback/OutputDeviceControl.h"
#include "playback/PlaybackTimeControl.h"
#include "playback/SeekControl.h"
#include "playback/TransportButton.h"
#include "playback/VolumeControl.h"
#include <ao/Error.h>
#include <ao/uimodel/layout/component/SharedLayoutComponentType.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/playback/command/PlaybackCommand.h>
#include <ao/uimodel/playback/seek/PlaybackPositionInteraction.h>

#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Windows.UI.Text.h>

#include <memory>
#include <string>
#include <string_view>

namespace ao::winui::layout
{
  namespace
  {
    using winrt::Microsoft::UI::Xaml::FrameworkElement;
    using winrt::Microsoft::UI::Xaml::ResourceDictionary;
    using winrt::Microsoft::UI::Xaml::Controls::Button;
    using winrt::Microsoft::UI::Xaml::Controls::ControlTemplate;
    using winrt::Microsoft::UI::Xaml::Controls::Flyout;
    using winrt::Microsoft::UI::Xaml::Controls::Orientation;
    using winrt::Microsoft::UI::Xaml::Controls::Slider;
    using winrt::Microsoft::UI::Xaml::Controls::StackPanel;
    using winrt::Microsoft::UI::Xaml::Controls::Symbol;
    using winrt::Microsoft::UI::Xaml::Controls::SymbolIcon;
    using winrt::Microsoft::UI::Xaml::Controls::TextBlock;
    using winrt::Microsoft::UI::Xaml::Controls::ToolTipService;
    using winrt::Microsoft::UI::Xaml::Controls::Primitives::FlyoutPlacementMode;

    constexpr auto kOverlayPresentation = std::string_view{"overlay"};
    constexpr auto kInlinePresentation = std::string_view{"inline"};
    constexpr auto kSeekThumbTemplateKey = std::string_view{"ModernSeekThumbTemplate"};
    constexpr auto kOverlaySeekChromeKey = std::string_view{"ModernSeekOverlayResources"};
    constexpr auto kInlineSeekChromeKey = std::string_view{"ClassicSeekInlineResources"};

    constexpr double kNormalizedMinimum = 0.0;
    constexpr double kNormalizedMaximum = 1.0;
    constexpr double kVolumeFlyoutWidth = 180.0;
    constexpr double kVolumeFlyoutSpacing = 6.0;

    uimodel::PlaybackCommand commandOf(uimodel::LayoutNode const& node)
    {
      auto const optCommand = uimodel::playbackCommandFor(node.propertyOr<std::string>(uimodel::kCommandProp, ""));
      return optCommand.value_or(uimodel::PlaybackCommand::PlayPause);
    }

    uimodel::PlaybackTimeMode timeModeOf(uimodel::LayoutNode const& node)
    {
      auto const mode = node.propertyOr<std::string>(uimodel::kModeProp, "combined");

      if (mode == "duration")
      {
        return uimodel::PlaybackTimeMode::Duration;
      }

      return mode == "elapsed" ? uimodel::PlaybackTimeMode::Elapsed : uimodel::PlaybackTimeMode::Combined;
    }

    /**
     * @brief One transport command, as a button.
     *
     * The glyph, tooltip, and enablement all follow the command's own view
     * state, so the component contributes the button and nothing else.
     */
    class TransportButtonComponent final : public LayoutComponent
    {
    public:
      TransportButtonComponent(LayoutBuildContext& ctx, uimodel::LayoutNode const& node)
        : _transport{
            TransportButtonConfig{.button = _button, .textCatalog = ctx.textCatalog, .command = commandOf(node)}}
      {
        _transport.bind(ctx.playback, ctx.playbackActions);
      }

      FrameworkElement element() const override { return _button; }

    private:
      Button _button{};
      TransportButton _transport;
    };

    /// The position slider, in the presentation the document asked for.
    class SeekSliderComponent final : public LayoutComponent
    {
    public:
      SeekSliderComponent(LayoutBuildContext& ctx, bool const overlay)
        : _seek{SeekControlConfig{
            .slider = configuredSlider(_slider),
            .modernOverlay = overlay,
            // The overlay thumb is a template, which a `Style` cannot carry, so
            // the frame hands it over the same way it hands over item templates.
            .thumbTemplate = adoptChrome(_slider, ctx.resources, overlay),
          }}
      {
        _seek.bind(ctx.playback);
      }

      FrameworkElement element() const override { return _slider; }

    private:
      /// The slider reports a fraction of the track; the control owns the real range.
      static Slider configuredSlider(Slider const& slider)
      {
        slider.Minimum(kNormalizedMinimum);
        slider.Maximum(kNormalizedMaximum);
        return slider;
      }

      /**
       * @brief Give @p slider the frame's chrome for its presentation.
       *
       * The stock Slider template reads its metrics from the element's own
       * resource scope upward, and the keys it reads are WinUI's own, so the
       * frame keeps each presentation's chrome behind a key of its own and this
       * gives it to the one slider that asked for it. Publishing those keys to
       * the window instead would re-chrome every slider in the shell.
       *
       * The entries are copied rather than the dictionary merged: a
       * `ResourceDictionary` can be the merged child of only one scope at a
       * time, and every generation builds a slider that wants the same one.
       * Copying shares the values, which is what a XAML resource is for.
       */
      static ControlTemplate adoptChrome(Slider const& slider, ResourceDictionary const& resources, bool const overlay)
      {
        auto const chrome = lookupResource(resources, overlay ? kOverlaySeekChromeKey : kInlineSeekChromeKey)
                              .try_as<ResourceDictionary>();

        if (!chrome)
        {
          // A slider with stock chrome still seeks, so a frame that ships none
          // is a presentation gap rather than a reason to fail the build.
          return nullptr;
        }

        for (auto const& entry : chrome)
        {
          slider.Resources().Insert(entry.Key(), entry.Value());
        }

        auto const thumbKey = winrt::box_value(winrt::to_hstring(kSeekThumbTemplateKey));
        return chrome.HasKey(thumbKey) ? chrome.Lookup(thumbKey).try_as<ControlTemplate>() : ControlTemplate{nullptr};
      }

      Slider _slider{};
      SeekControl _seek;
    };

    /// The clock beside the seek slider, in one of its three readings.
    class TimeLabelComponent final : public LayoutComponent
    {
    public:
      TimeLabelComponent(LayoutBuildContext& ctx, uimodel::LayoutNode const& node)
        : _time{PlaybackTimeControlConfig{.text = _text, .mode = timeModeOf(node)}}
      {
        _time.bind(ctx.playback);
      }

      FrameworkElement element() const override { return _text; }

    private:
      TextBlock _text{};
      PlaybackTimeControl _time;
    };

    /**
     * @brief The volume slider, either shown inline or behind a button.
     *
     * Both presentations are the same slider: only who holds it differs, so the
     * flyout variant returns the button and keeps the slider in its content.
     */
    class VolumeControlComponent final : public LayoutComponent
    {
    public:
      VolumeControlComponent(LayoutBuildContext& ctx, bool const flyout)
        : _volume{VolumeControlConfig{.slider = configuredSlider(_slider), .textCatalog = ctx.textCatalog}}
      {
        if (flyout)
        {
          auto heading = TextBlock{};
          heading.Text(winrt::to_hstring(resourceString("winui_playback_volume")));
          heading.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());

          auto content = StackPanel{};
          content.Orientation(Orientation::Vertical);
          content.Width(kVolumeFlyoutWidth);
          content.Spacing(kVolumeFlyoutSpacing);
          content.Children().Append(heading);
          content.Children().Append(_slider);

          auto popup = Flyout{};
          popup.Placement(FlyoutPlacementMode::Top);
          popup.Content(content);

          _button = Button{};
          _button.Content(SymbolIcon{Symbol::Volume});
          _button.Flyout(popup);
          ToolTipService::SetToolTip(_button, winrt::box_value(resourceHstring(L"winui_playback_volume")));
        }

        _volume.bind(ctx.playback);
      }

      FrameworkElement element() const override
      {
        return _button ? FrameworkElement{_button} : FrameworkElement{_slider};
      }

    private:
      static Slider configuredSlider(Slider const& slider)
      {
        slider.Minimum(kNormalizedMinimum);
        slider.Maximum(kNormalizedMaximum);
        slider.Value(kNormalizedMaximum);
        return slider;
      }

      Slider _slider{};
      Button _button{nullptr};
      VolumeControl _volume;
    };

    /// The button that names the active output backend and offers the others.
    class OutputDeviceButtonComponent final : public LayoutComponent
    {
    public:
      explicit OutputDeviceButtonComponent(LayoutBuildContext& ctx)
        : _outputDevice{OutputDeviceControlConfig{
            .presenter = _button,
            .intent = ctx.outputDeviceIntent,
            .textCatalog = ctx.textCatalog,
          }}
      {
        _outputDevice.bind(ctx.playback);
      }

      FrameworkElement element() const override { return _button; }

    private:
      Button _button{};
      OutputDeviceControl _outputDevice;
    };
  } // namespace

  void registerPlaybackComponents(ComponentRegistry& registry)
  {
    registry.registerComponent(
      uimodel::componentTypeName(uimodel::SharedLayoutComponentType::PlaybackTransportButton),
      [](LayoutBuildContext& ctx, uimodel::LayoutNode const& node) -> Result<std::unique_ptr<LayoutComponent>>
      { return std::make_unique<TransportButtonComponent>(ctx, node); });

    registry.registerComponent(uimodel::componentTypeName(uimodel::SharedLayoutComponentType::PlaybackSoulButton),
                               [](LayoutBuildContext& ctx, uimodel::LayoutNode const& node)
                               { return makeSoulButton(ctx, node); });

    registry.registerComponent(
      uimodel::componentTypeName(uimodel::SharedLayoutComponentType::PlaybackSeekSlider),
      [](LayoutBuildContext& ctx, uimodel::LayoutNode const& node) -> Result<std::unique_ptr<LayoutComponent>>
      {
        return std::make_unique<SeekSliderComponent>(
          ctx, node.propertyOr<std::string>("presentation", std::string{kInlinePresentation}) == kOverlayPresentation);
      });

    registry.registerComponent(
      uimodel::componentTypeName(uimodel::SharedLayoutComponentType::PlaybackTimeLabel),
      [](LayoutBuildContext& ctx, uimodel::LayoutNode const& node) -> Result<std::unique_ptr<LayoutComponent>>
      { return std::make_unique<TimeLabelComponent>(ctx, node); });

    registry.registerComponent(
      uimodel::componentTypeName(uimodel::SharedLayoutComponentType::PlaybackVolumeControl),
      [](LayoutBuildContext& ctx, uimodel::LayoutNode const& node) -> Result<std::unique_ptr<LayoutComponent>>
      {
        return std::make_unique<VolumeControlComponent>(
          ctx, node.propertyOr<std::string>("presentation", "flyout") != kInlinePresentation);
      });

    registry.registerComponent(
      uimodel::componentTypeName(uimodel::SharedLayoutComponentType::PlaybackOutputDeviceSelector),
      [](LayoutBuildContext& ctx, uimodel::LayoutNode const& /*node*/) -> Result<std::unique_ptr<LayoutComponent>>
      { return std::make_unique<OutputDeviceButtonComponent>(ctx); });

    registry.registerComponent("playback.nowPlayingInfo",
                               [](LayoutBuildContext& ctx, uimodel::LayoutNode const& node)
                               { return makeNowPlayingInfo(ctx, node); });
  }
} // namespace ao::winui::layout
