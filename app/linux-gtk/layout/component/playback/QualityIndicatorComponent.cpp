// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "PlaybackComponentRegistrations.h"
#include "app/AobusSoul.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include <ao/rt/AppRuntime.h>
#include <ao/uimodel/layout/action/LayoutActionSlot.h>
#include <ao/uimodel/layout/component/LayoutComponentActionPolicy.h>
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/playback/soul/AobusSoulViewModel.h>

#include <gtkmm/widget.h>

#include <memory>

namespace ao::gtk::layout
{
  using namespace uimodel;
  namespace
  {
    using uimodel::LayoutComponentActionPolicy;
    using uimodel::slotBit;

    /**
     * @brief playback.qualityIndicator
     */
    class QualityIndicatorComponent final : public LayoutComponent
    {
    public:
      QualityIndicatorComponent(LayoutBuildContext& ctx, LayoutNode const& /*node*/)
        : _runtime{ctx.runtime}
        , _soulViewModel{_runtime.playback(),
                         [this](uimodel::AobusSoulViewState const& view)
                         {
                           _soul.breathe(view.isBreathing);
                           _soul.setAura(AobusSoul::mapSoulAura(view.aura));
                         }}
      {
      }

      Gtk::Widget& widget() override { return _soul; }

    private:
      rt::AppRuntime& _runtime;
      AobusSoul _soul{};
      uimodel::AobusSoulViewModel _soulViewModel;
    };

    std::unique_ptr<LayoutComponent> createQualityIndicator(LayoutBuildContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<QualityIndicatorComponent>(ctx, node);
    }
  } // namespace

  void registerQualityIndicatorComponent(ComponentRegistry& registry)
  {
    registry.registerComponent(
      {.type = "playback.qualityIndicator",
       .displayName = "Quality Indicator",
       .category = LayoutComponentCategory::Playback,
       .minChildren = 0,
       .optMaxChildren = 0,
       .actionPolicy =
         LayoutComponentActionPolicy{
           .slotMask = slotBit(LayoutActionSlot::SecondaryClick) | slotBit(LayoutActionSlot::SecondaryLongPress),
           .defaultActionIds = {{LayoutActionSlot::SecondaryLongPress, "shell.showSoul"}}}},
      createQualityIndicator);
  }
} // namespace ao::gtk::layout
