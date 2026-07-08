// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "PlaybackComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutComponent.h"
#include "layout/runtime/LayoutContext.h"
#include "playback/VolumeControl.h"
#include <ao/rt/AppRuntime.h>
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <gtkmm/enums.h>
#include <gtkmm/widget.h>

#include <memory>
#include <string>

namespace ao::gtk::layout
{
  using namespace uimodel;
  namespace
  {
    /**
     * @brief playback.volumeControl
     */
    class VolumeControlComponent final : public LayoutComponent
    {
    public:
      VolumeControlComponent(LayoutContext& ctx, LayoutNode const& node)
        : _control{ctx.runtime.playback()}
      {
        auto const orient = node.propertyOr<std::string>("orientation", "horizontal");

        if (orient == "vertical")
        {
          _control.setOrientation(Gtk::Orientation::VERTICAL);
        }
      }

      Gtk::Widget& widget() override { return _control.widget(); }

    private:
      VolumeControl _control;
    };

    std::unique_ptr<LayoutComponent> createVolumeControl(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<VolumeControlComponent>(ctx, node);
    }
  } // namespace

  void registerVolumeControlComponent(ComponentRegistry& registry)
  {
    registry.registerComponent({.type = "playback.volumeControl",
                                .displayName = "Volume Control",
                                .category = LayoutComponentCategory::Playback,
                                .props = {},
                                .layoutProps = {},
                                .minChildren = 0,
                                .optMaxChildren = 0},
                               createVolumeControl);
  }
} // namespace ao::gtk::layout
