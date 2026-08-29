// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "layout/component/ComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include "playback/AudioPipelinePanel.h"
#include <ao/rt/playback/PlaybackService.h>
#include <ao/uimodel/layout/component/LayoutSchema.h>
#include <ao/uimodel/layout/component/LayoutSurface.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/playback/now-playing/NowPlayingViewModel.h>

#include <gtkmm/widget.h>

#include <memory>
#include <string>

namespace ao::gtk::layout
{
  using namespace uimodel;
  namespace
  {
    /**
     * @brief playback.audioPipelinePanel
     */
    class AudioPipelinePanelComponent final : public LayoutComponent
    {
    public:
      AudioPipelinePanelComponent(rt::PlaybackService& playback,
                                  i18n::MessageCatalog const& textCatalog,
                                  LayoutBuildContext const& ctx,
                                  LayoutNode const& node)
        : _panel{textCatalog,
                 [](LayoutBuildContext const& ctx2, LayoutNode const& n)
                 {
                   auto const variantStr = n.propertyOr<std::string>("variant", "");

                   if (variantStr == "inline")
                   {
                     return AudioPipelinePanelVariant::Inline;
                   }

                   if (variantStr == "compact")
                   {
                     return AudioPipelinePanelVariant::Compact;
                   }

                   if (variantStr == "tooltip")
                   {
                     return AudioPipelinePanelVariant::Tooltip;
                   }

                   return ctx2.surface == uimodel::LayoutSurface::Tooltip ? AudioPipelinePanelVariant::Tooltip
                                                                          : AudioPipelinePanelVariant::Inline;
                 }(ctx, node)}
        , _viewModel{playback, textCatalog, [this](auto const& view) { _panel.apply(view.audioPipeline); }}
      {
      }

      Gtk::Widget& widget() override { return _panel; }

    private:
      AudioPipelinePanel _panel;
      uimodel::NowPlayingViewModel _viewModel;
    };
  } // namespace

  void registerAudioPipelinePanelComponent(ComponentRegistry& registry,
                                           rt::PlaybackService& playback,
                                           i18n::MessageCatalog const& textCatalog)
  {
    registry.registerComponent(
      {.id = "playback.audioPipelinePanel",
       .displayName = "Audio Pipeline Panel",
       .category = ComponentCategory::Playback,
       .properties = {{.name = "variant",
                       .kind = PropertyKind::Enum,
                       .label = "Variant",
                       .defaultValue = LayoutValue{""},
                       .enumValues = {"inline", "compact", "tooltip"}}},
       .minChildren = 0,
       .optMaxChildren = 0,
       .surfaces = static_cast<uimodel::LayoutSurfaceCapabilityMask>(uimodel::LayoutSurfaceCapability::Main) |
                   static_cast<uimodel::LayoutSurfaceCapabilityMask>(uimodel::LayoutSurfaceCapability::Tooltip)},
      [&playback, textCatalog](LayoutBuildContext const& ctx, LayoutNode const& node)
      { return std::make_unique<AudioPipelinePanelComponent>(playback, textCatalog, ctx, node); });
  }
} // namespace ao::gtk::layout
