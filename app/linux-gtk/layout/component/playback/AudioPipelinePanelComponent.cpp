// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "PlaybackComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include "playback/AudioPipelinePanel.h"
#include <ao/rt/AppRuntime.h>
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
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
      AudioPipelinePanelComponent(LayoutBuildContext& ctx, LayoutNode const& node)
        : _panel{[](LayoutBuildContext const& ctx2, LayoutNode const& n)
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

                   return ctx2.surface == LayoutSurface::Tooltip ? AudioPipelinePanelVariant::Tooltip
                                                                 : AudioPipelinePanelVariant::Inline;
                 }(ctx, node)}
        , _viewModel{ctx.runtime.playback(), [this](auto const& view) { _panel.apply(view.audioPipeline); }}
      {
      }

      Gtk::Widget& widget() override { return _panel; }

    private:
      AudioPipelinePanel _panel;
      uimodel::NowPlayingViewModel _viewModel;
    };

    std::unique_ptr<LayoutComponent> createAudioPipelinePanel(LayoutBuildContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<AudioPipelinePanelComponent>(ctx, node);
    }
  } // namespace

  void registerAudioPipelinePanelComponent(ComponentRegistry& registry)
  {
    registry.registerComponent(
      {.type = "playback.audioPipelinePanel",
       .displayName = "Audio Pipeline Panel",
       .category = LayoutComponentCategory::Playback,
       .props = {{.name = "variant",
                  .kind = LayoutPropertyKind::Enum,
                  .label = "Variant",
                  .defaultValue = LayoutValue{""},
                  .enumValues = {"inline", "compact", "tooltip"}}},
       .layoutProps = {},
       .minChildren = 0,
       .optMaxChildren = 0,
       .surfaces = static_cast<uimodel::LayoutSurfaceCapabilityMask>(uimodel::LayoutSurfaceCapability::Main) |
                   static_cast<uimodel::LayoutSurfaceCapabilityMask>(uimodel::LayoutSurfaceCapability::Tooltip)},
      createAudioPipelinePanel);
  }
} // namespace ao::gtk::layout
