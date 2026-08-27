// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "PlaybackComponentRegistrations.h"
#include "app/AobusSoul.h"
#include "common/AccessibleLabel.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/uimodel/layout/component/LayoutComponentActionPolicy.h>
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
#include <ao/uimodel/layout/component/SharedLayoutComponentType.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/playback/soul/AobusSoulViewModel.h>

#include <gtkmm/button.h>
#include <gtkmm/enums.h>
#include <gtkmm/widget.h>

#include <memory>
#include <string>

namespace ao::gtk::layout
{
  namespace
  {
    /**
     * @brief Which static ornament this shell's soul wears.
     *
     * Not part of the shared vocabulary: the Windows soul draws the live
     * transport icon and only decides whether to show it, so the same name
     * there answers a different question. Each shell spells its own.
     */
    constexpr auto kSoulGlyphProp = std::string_view{"glyph"};
  } // namespace

  using namespace uimodel;
  namespace
  {
    using uimodel::kAllExternalActions;

    /**
     * @brief playback.soulButton
     */
    class SoulButtonComponent final : public LayoutComponent
    {
    public:
      SoulButtonComponent(rt::PlaybackService& playback, LayoutNode const& node)
        : _soulViewModel{playback,
                         [this](uimodel::AobusSoulViewState const& state)
                         {
                           _soul.setMotionMode(state.motionMode);
                           _soul.setAura(AobusSoul::mapSoulAura(state.aura));
                         }}
      {
        _button.set_has_frame(false);
        _button.add_css_class("ao-soul-button");
        _button.set_child(_soul);
        setAccessibleLabel(_button, "Aobus Soul");

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

        auto const glyph = node.propertyOr<std::string>(kSoulGlyphProp, "none");

        if (glyph == "sigil")
        {
          _soul.setInnerGlyph(AobusSoul::InnerGlyph::Sigil);
        }
        else if (glyph == "seal")
        {
          _soul.setInnerGlyph(AobusSoul::InnerGlyph::Seal);
        }

        _soul.setShowFullLogo(node.propertyOr<bool>("showFullLogo", false));
      }

      Gtk::Widget& widget() override { return _button; }

    private:
      Gtk::Button _button;
      AobusSoul _soul;
      uimodel::AobusSoulViewModel _soulViewModel;
    };
  } // namespace

  void registerSoulButtonComponent(ComponentRegistry& registry, rt::PlaybackService& playback)
  {
    registry.registerComponent(
      // GDK tells a secondary hold apart from a primary one, which Windows
      // cannot, so the slot is this shell's own extension rather than shared.
      withShellActionSlots(withShellProperties(sharedComponentDescriptor(SharedLayoutComponentType::PlaybackSoulButton),
                                               {// Which of two static ornaments the soul wears. The
                                                // Windows soul draws the live transport icon instead,
                                                // so this names a concept only this shell has.
                                                {.name = std::string{kSoulGlyphProp},
                                                 .kind = LayoutPropertyKind::Enum,
                                                 .label = "Glyph",
                                                 .defaultValue = LayoutValue{"none"},
                                                 .enumValues = {"none", "sigil", "seal"}},
                                                {.name = "showFullLogo",
                                                 .kind = LayoutPropertyKind::Bool,
                                                 .label = "Show Full Logo",
                                                 .defaultValue = LayoutValue{false}}}),
                           kAllExternalActions),
      [&playback](LayoutBuildContext const& /*ctx*/, LayoutNode const& node)
      { return std::make_unique<SoulButtonComponent>(playback, node); });
  }
} // namespace ao::gtk::layout
