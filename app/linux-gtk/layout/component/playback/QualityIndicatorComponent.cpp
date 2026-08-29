// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "app/AobusSoul.h"
#include "layout/component/ComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include <ao/rt/playback/PlaybackService.h>
#include <ao/uimodel/layout/component/LayoutSchema.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/playback/soul/AobusSoulViewModel.h>

#include <gtkmm/widget.h>

#include <memory>

namespace ao::gtk::layout
{
  using namespace uimodel;
  namespace
  {
    using uimodel::actionSlotBit;

    /**
     * @brief playback.qualityIndicator
     */
    class QualityIndicatorComponent final : public LayoutComponent
    {
    public:
      explicit QualityIndicatorComponent(rt::PlaybackService& playback)
        : _soulViewModel{playback,
                         [this](uimodel::AobusSoulViewState const& view)
                         {
                           _soul.setMotionMode(view.motionMode);
                           _soul.setAura(AobusSoul::mapSoulAura(view.aura));
                         }}
      {
      }

      Gtk::Widget& widget() override { return _soul; }

    private:
      AobusSoul _soul{};
      uimodel::AobusSoulViewModel _soulViewModel;
    };
  } // namespace

  void registerQualityIndicatorComponent(ComponentRegistry& registry, rt::PlaybackService& playback)
  {
    registry.registerComponent(
      {.id = "playback.qualityIndicator",
       .displayName = "Quality Indicator",
       .category = ComponentCategory::Playback,
       .minChildren = 0,
       .optMaxChildren = 0,
       .actionSlots = actionSlotBit(ActionSlot::SecondaryClick) | actionSlotBit(ActionSlot::SecondaryLongPress),
       .defaultActions = {{.slot = ActionSlot::SecondaryLongPress, .actionId = "shell.showSoul"}}},
      [&playback](LayoutBuildContext const& /*ctx*/, LayoutNode const& /*node*/)
      { return std::make_unique<QualityIndicatorComponent>(playback); });
  }
} // namespace ao::gtk::layout
