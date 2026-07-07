// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "PlaybackComponentRegistrations.h"
#include "app/AobusSoul.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/ILayoutComponent.h"
#include "layout/runtime/LayoutContext.h"
#include <ao/rt/AppRuntime.h>
#include <ao/uimodel/layout/component/LayoutComponentActionPolicy.h>
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/playback/soul/AobusSoulViewModel.h>

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
    using uimodel::kAllExternalActions;

    constexpr double kDefaultStrokeWidth = 9.0;

    /**
     * @brief playback.soulButton
     */
    class SoulButtonComponent final : public ILayoutComponent
    {
    public:
      SoulButtonComponent(LayoutContext& ctx, LayoutNode const& node)
        : _soulController{ctx.runtime.playback(),
                          [this](uimodel::AobusSoulViewState const& state)
                          {
                            _soul.breathe(state.isBreathing);
                            _soul.setAura(AobusSoul::mapSoulAura(state.aura));
                          }}
      {
        _button.set_has_frame(false);
        _button.add_css_class("ao-soul-button");
        _button.set_child(_soul);

        _soul.set_halign(Gtk::Align::FILL);
        _soul.set_valign(Gtk::Align::FILL);

        if (auto const strokeWidth = node.getProp<double>("strokeWidth", 0.0); strokeWidth > 0.0)
        {
          _soul.setBaseStrokeWidth(static_cast<float>(strokeWidth));
        }

        if (auto const glyphScale = node.getProp<double>("glyphScale", 0.0); glyphScale > 0.0)
        {
          _soul.setInnerGlyphScale(static_cast<float>(glyphScale));
        }

        auto const glyph = node.getProp<std::string>("glyph", "none");

        if (glyph == "sigil")
        {
          _soul.setInnerGlyph(AobusSoul::InnerGlyph::Sigil);
        }
        else if (glyph == "seal")
        {
          _soul.setInnerGlyph(AobusSoul::InnerGlyph::Seal);
        }

        _soul.setShowFullLogo(node.getProp<bool>("showFullLogo", false));
      }

      Gtk::Widget& widget() override { return _button; }

    private:
      Gtk::Button _button;
      AobusSoul _soul;
      uimodel::AobusSoulViewModel _soulController;
    };

    std::unique_ptr<ILayoutComponent> createSoulButton(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<SoulButtonComponent>(ctx, node);
    }
  } // namespace

  void registerSoulButtonComponent(ComponentRegistry& registry)
  {
    registry.registerComponent({.type = "playback.soulButton",
                                .displayName = "Soul Button",
                                .category = LayoutComponentCategory::Playback,
                                .props = {{.name = "strokeWidth",
                                           .kind = LayoutPropertyKind::Double,
                                           .label = "Stroke Width",
                                           .defaultValue = LayoutValue{kDefaultStrokeWidth}},
                                          {.name = "glyph",
                                           .kind = LayoutPropertyKind::Enum,
                                           .label = "Glyph",
                                           .defaultValue = LayoutValue{"none"},
                                           .enumValues = {"none", "sigil", "seal"}},
                                          {.name = "glyphScale",
                                           .kind = LayoutPropertyKind::Double,
                                           .label = "Glyph Scale",
                                           .defaultValue = LayoutValue{1.0}},
                                          {.name = "showFullLogo",
                                           .kind = LayoutPropertyKind::Bool,
                                           .label = "Show Full Logo",
                                           .defaultValue = LayoutValue{false}}},
                                .layoutProps = {},
                                .minChildren = 0,
                                .optMaxChildren = 0,
                                .actionPolicy = kAllExternalActions},
                               createSoulButton);
  }
} // namespace ao::gtk::layout
