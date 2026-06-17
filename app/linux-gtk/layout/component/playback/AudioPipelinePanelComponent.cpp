// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "PlaybackComponentRegistrations.h"
#include "layout/document/LayoutNode.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/ILayoutComponent.h"
#include "layout/runtime/LayoutContext.h"
#include "playback/AudioPipelinePanel.h"
#include <ao/rt/AppRuntime.h>
#include <ao/uimodel/layout/ComponentCatalog.h>
#include <ao/uimodel/playback/NowPlayingViewModel.h>

#include <gtkmm/widget.h>

#include <memory>
#include <string>

namespace ao::gtk::layout
{
  namespace
  {
    /**
     * @brief playback.audioPipelinePanel
     */
    class AudioPipelinePanelComponent final : public ILayoutComponent
    {
    public:
      AudioPipelinePanelComponent(LayoutContext& ctx, LayoutNode const& node)
        : _panel{[](LayoutContext const& ctx2, LayoutNode const& n)
                 {
                   auto const variantStr = n.getProp<std::string>("variant", "");

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
      uimodel::playback::NowPlayingViewModel _viewModel;
    };

    std::unique_ptr<ILayoutComponent> createAudioPipelinePanel(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<AudioPipelinePanelComponent>(ctx, node);
    }
  } // namespace

  void registerAudioPipelinePanelComponent(ComponentRegistry& registry)
  {
    registry.registerComponent(
      {.type = "playback.audioPipelinePanel",
       .displayName = "Audio Pipeline Panel",
       .category = ComponentCategory::Playback,
       .props = {{.name = "variant",
                  .kind = PropertyKind::Enum,
                  .label = "Variant",
                  .defaultValue = LayoutValue{""},
                  .enumValues = {"inline", "compact", "tooltip"}}},
       .layoutProps = {},
       .minChildren = 0,
       .optMaxChildren = 0,
       .surfaces = static_cast<uimodel::layout::SurfaceCapabilityMask>(uimodel::layout::SurfaceCapability::Main) |
                   static_cast<uimodel::layout::SurfaceCapabilityMask>(uimodel::layout::SurfaceCapability::Tooltip)},
      createAudioPipelinePanel);
  }
} // namespace ao::gtk::layout
