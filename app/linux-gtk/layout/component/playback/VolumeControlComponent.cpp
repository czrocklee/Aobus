// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "PlaybackComponentRegistrations.h"
#include "app/GtkUiDependencies.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include "playback/VolumeControlWidget.h"
#include <ao/rt/AppRuntime.h>
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
#include <ao/uimodel/layout/component/SharedLayoutComponentType.h>
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
      VolumeControlComponent(LayoutBuildContext& ctx, LayoutNode const& node)
        : _control{ctx.runtime.playback(), ctx.dependencies.textCatalog, ctx.dependencies.gtkTextCatalog}
      {
        auto const orient = node.propertyOr<std::string>(kOrientationProp, "horizontal");

        if (orient == "vertical")
        {
          _control.setOrientation(Gtk::Orientation::VERTICAL);
        }
      }

      Gtk::Widget& widget() override { return _control.widget(); }

    private:
      VolumeControlWidget _control;
    };

    std::unique_ptr<LayoutComponent> createVolumeControl(LayoutBuildContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<VolumeControlComponent>(ctx, node);
    }
  } // namespace

  void registerVolumeControlComponent(ComponentRegistry& registry)
  {
    // GTK can stand the slider on end, which no other shell offers yet, and
    // which the descriptor has to admit for a document to reach the code below.
    registry.registerComponent(
      withShellProperties(sharedComponentDescriptor(SharedLayoutComponentType::PlaybackVolumeControl),
                          {{.name = std::string{kOrientationProp},
                            .kind = LayoutPropertyKind::Enum,
                            .label = "Orientation",
                            .defaultValue = LayoutValue{"horizontal"},
                            .enumValues = {"horizontal", "vertical"}}}),
      createVolumeControl);
  }
} // namespace ao::gtk::layout
