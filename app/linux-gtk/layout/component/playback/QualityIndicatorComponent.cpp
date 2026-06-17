// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "PlaybackComponentRegistrations.h"
#include "app/AobusSoul.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/ILayoutComponent.h"
#include "layout/runtime/LayoutContext.h"
#include <ao/rt/AppRuntime.h>
#include <ao/uimodel/layout/ActionTypes.h>
#include <ao/uimodel/layout/ComponentActionPolicy.h>
#include <ao/uimodel/layout/ComponentCatalog.h>
#include <ao/uimodel/layout/LayoutNode.h>
#include <ao/uimodel/playback/AobusSoulViewModel.h>

#include <gtkmm/widget.h>

#include <memory>

namespace ao::gtk::layout
{
  using namespace uimodel::layout;
  namespace
  {
    using uimodel::layout::ComponentActionPolicy;
    using uimodel::layout::slotBit;

    /**
     * @brief playback.qualityIndicator
     */
    class QualityIndicatorComponent final : public ILayoutComponent
    {
    public:
      QualityIndicatorComponent(LayoutContext& ctx, LayoutNode const& /*node*/)
        : _runtime{ctx.runtime}
        , _soulController{_runtime.playback(),
                          [this](uimodel::playback::AobusSoulViewState const& view)
                          {
                            _soul.breathe(view.isBreathing);
                            _soul.setAura(AobusSoul::mapAuraColor(view.auraColor));
                          }}
      {
      }

      Gtk::Widget& widget() override { return _soul; }

    private:
      rt::AppRuntime& _runtime;
      AobusSoul _soul{};
      uimodel::playback::AobusSoulViewModel _soulController;
    };

    std::unique_ptr<ILayoutComponent> createQualityIndicator(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<QualityIndicatorComponent>(ctx, node);
    }
  } // namespace

  void registerQualityIndicatorComponent(ComponentRegistry& registry)
  {
    registry.registerComponent(
      {.type = "playback.qualityIndicator",
       .displayName = "Quality Indicator",
       .category = ComponentCategory::Playback,
       .props = {},
       .layoutProps = {},
       .minChildren = 0,
       .optMaxChildren = 0,
       .actionPolicy = ComponentActionPolicy{.slotMask = slotBit(ActionSlot::SecondaryClick) |
                                                         slotBit(ActionSlot::SecondaryLongPress),
                                             .defaultActionIds = {{ActionSlot::SecondaryLongPress, "shell.showSoul"}}}},
      createQualityIndicator);
  }
} // namespace ao::gtk::layout
