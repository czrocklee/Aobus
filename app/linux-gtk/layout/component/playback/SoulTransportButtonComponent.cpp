// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "PlaybackComponentRegistrations.h"
#include "app/AobusSoul.h"
#include "common/AccessibleLabel.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include "playback/TransportButton.h"
#include <ao/Contract.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/uimodel/layout/action/LayoutActionSlot.h>
#include <ao/uimodel/layout/component/LayoutComponentActionPolicy.h>
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
#include <ao/uimodel/layout/component/SharedLayoutComponentType.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/playback/soul/AobusSoulViewModel.h>
#include <ao/uimodel/playback/transport/TransportViewModel.h>

#include <gtkmm/button.h>
#include <gtkmm/enums.h>
#include <gtkmm/widget.h>

#include <memory>
#include <string>

namespace ao::gtk::layout
{
  using namespace uimodel;
  namespace
  {
    using uimodel::LayoutComponentActionPolicy;
    using uimodel::slotBit;

    constexpr double kDefaultStrokeWidth = uimodel::kAobusSoulGeometry.baseStrokeWidth;

    uimodel::PlaybackCommandSurface& commandSurface(uimodel::PlaybackCommandSurface* playbackCommandSurface)
    {
      AO_EXPECTS(
        playbackCommandSurface != nullptr, "SoulTransportButtonComponent: playback command surface is not bound");

      return *playbackCommandSurface;
    }

    /**
     * @brief playback.soulPlayPauseButton
     */
    class SoulTransportButtonComponent final : public LayoutComponent
    {
    public:
      SoulTransportButtonComponent(rt::PlaybackService& playback,
                                   uimodel::PlaybackCommandSurface* playbackCommands,
                                   i18n::MessageCatalog const& textCatalog,
                                   LayoutNode const& node)
        : _hasComplexTooltip{node.optTooltip.has_value()}
        , _transportViewModel{playback,
                              commandSurface(playbackCommands),
                              textCatalog,
                              TransportButton::Action::PlayPause,
                              false,
                              [this](uimodel::TransportViewState const& state) { applyTransportState(state); }}
        , _soulViewModel{playback, [this](uimodel::AobusSoulViewState const& state) { applySoulState(state); }}
      {
        _button.set_child(_soul);
        _button.set_has_frame(false);
        _button.add_css_class("ao-soul-button");
        _button.set_valign(Gtk::Align::CENTER);
        _button.set_halign(Gtk::Align::CENTER);

        _soul.set_halign(Gtk::Align::FILL);
        _soul.set_valign(Gtk::Align::FILL);

        if (auto const strokeWidth = node.propertyOr<double>(kStrokeWidthProp, 0.0); strokeWidth > 0.0)
        {
          _soul.setBaseStrokeWidth(static_cast<float>(strokeWidth));
        }

        if (auto const glyphScale = node.propertyOr<double>(kGlyphScaleProp, 0.0); glyphScale > 0.0)
        {
          _soul.setInnerGlyphScale(static_cast<float>(glyphScale));
        }

        _button.signal_clicked().connect([this] { _transportViewModel.handleClick(); });
      }

      Gtk::Widget& widget() override { return _button; }

    private:
      void applyTransportState(uimodel::TransportViewState const& view)
      {
        using Icon = uimodel::TransportIcon;

        if (view.icon == Icon::Pause)
        {
          _soul.setInnerGlyph(AobusSoul::InnerGlyph::Seal);
        }
        else if (view.icon == Icon::Play)
        {
          _soul.setInnerGlyph(AobusSoul::InnerGlyph::Sigil);
        }
        else
        {
          _soul.setInnerGlyph(AobusSoul::InnerGlyph::None);
        }

        _button.set_sensitive(view.enabled);
        setAccessibleLabel(_button, view.tooltip);

        if (!_hasComplexTooltip)
        {
          _button.set_tooltip_text(view.tooltip);
        }
      }

      void applySoulState(uimodel::AobusSoulViewState const& view)
      {
        _soul.setMotionMode(view.motionMode);
        _soul.setAura(AobusSoul::mapSoulAura(view.aura));
      }

      Gtk::Button _button;
      AobusSoul _soul;
      // Initialized before the ViewModel's synchronous initial-state callback.
      bool _hasComplexTooltip = false;
      uimodel::TransportViewModel _transportViewModel;
      uimodel::AobusSoulViewModel _soulViewModel;
    };
  } // namespace

  void registerSoulTransportButtonComponent(ComponentRegistry& registry,
                                            rt::PlaybackService& playback,
                                            uimodel::PlaybackCommandSurface* playbackCommandSurface,
                                            i18n::MessageCatalog const& textCatalog)
  {
    registry.registerComponent(
      {.type = "playback.soulPlayPauseButton",
       .displayName = "Soul Play/Pause Button",
       .category = LayoutComponentCategory::Playback,
       .props = {{.name = std::string{kStrokeWidthProp},
                  .kind = LayoutPropertyKind::Double,
                  .label = "Stroke Width",
                  .defaultValue = LayoutValue{kDefaultStrokeWidth}},
                 {.name = std::string{kGlyphScaleProp},
                  .kind = LayoutPropertyKind::Double,
                  .label = "Glyph Scale",
                  .defaultValue = LayoutValue{1.0}}},
       .minChildren = 0,
       .optMaxChildren = 0,
       .actionPolicy =
         LayoutComponentActionPolicy{
           .slotMask = slotBit(LayoutActionSlot::SecondaryClick) | slotBit(LayoutActionSlot::SecondaryLongPress),
           .defaultActionIds = {{LayoutActionSlot::SecondaryLongPress, "shell.showSoul"}}}},
      [&playback, playbackCommandSurface, textCatalog](LayoutBuildContext const& /*ctx*/, LayoutNode const& node)
      { return std::make_unique<SoulTransportButtonComponent>(playback, playbackCommandSurface, textCatalog, node); });
  }
} // namespace ao::gtk::layout
