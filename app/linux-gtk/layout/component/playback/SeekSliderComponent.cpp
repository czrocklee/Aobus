// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "PlaybackComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include "playback/SeekControlWidget.h"
#include <ao/rt/AppRuntime.h>
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <gtkmm/widget.h>

#include <memory>

namespace ao::gtk::layout
{
  using namespace uimodel;
  namespace
  {
    /**
     * @brief playback.seekSlider
     */
    class SeekSliderComponent final : public LayoutComponent
    {
    public:
      SeekSliderComponent(LayoutBuildContext& ctx, LayoutNode const& /*node*/)
        : _control{ctx.runtime.playback()}
      {
      }

      Gtk::Widget& widget() override { return _control.widget(); }

    private:
      SeekControlWidget _control;
    };

    std::unique_ptr<LayoutComponent> createSeekSlider(LayoutBuildContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<SeekSliderComponent>(ctx, node);
    }
  } // namespace

  void registerSeekSliderComponent(ComponentRegistry& registry)
  {
    registry.registerComponent({.type = "playback.seekSlider",
                                .displayName = "Seek Slider",
                                .category = LayoutComponentCategory::Playback,
                                .props = {},
                                .layoutProps = {},
                                .minChildren = 0,
                                .optMaxChildren = 0},
                               createSeekSlider);
  }
} // namespace ao::gtk::layout
