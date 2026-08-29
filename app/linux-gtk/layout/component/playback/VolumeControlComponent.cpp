// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "layout/component/ComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include "playback/VolumeControlWidget.h"
#include <ao/rt/playback/PlaybackService.h>
#include <ao/uimodel/layout/component/LayoutSchema.h>
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
      VolumeControlComponent(rt::PlaybackService& playback,
                             i18n::MessageCatalog const& textCatalog,
                             LayoutNode const& node)
        : _control{playback, textCatalog}
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
  } // namespace

  void registerVolumeControlComponent(ComponentRegistry& registry,
                                      rt::PlaybackService& playback,
                                      i18n::MessageCatalog const& textCatalog)
  {
    // GTK can stand the slider on end, which no other shell offers yet, and
    // which the schema entry has to admit for a document to reach the code below.
    registry.registerSharedComponent("playback.volumeControl",
                                     {.properties = {{.name = std::string{kOrientationProp},
                                                      .kind = PropertyKind::Enum,
                                                      .label = "Orientation",
                                                      .defaultValue = LayoutValue{"horizontal"},
                                                      .enumValues = {"horizontal", "vertical"}}}},
                                     [&playback, textCatalog](LayoutBuildContext const& /*ctx*/, LayoutNode const& node)
                                     { return std::make_unique<VolumeControlComponent>(playback, textCatalog, node); });
  }
} // namespace ao::gtk::layout
