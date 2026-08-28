// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include "track/TrackPresentationButton.h"
#include <ao/rt/AppRuntime.h>
#include <ao/uimodel/layout/component/LayoutSchema.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <gtkmm/widget.h>

#include <memory>

namespace ao::gtk::layout
{
  using namespace uimodel;
  namespace
  {
    /**
     * @brief track.presentationButton component wrapper
     */
    class TrackPresentationButtonComponent final : public LayoutComponent
    {
    public:
      TrackPresentationButtonComponent(rt::AppRuntime& runtime,
                                       uimodel::TrackPresentationCatalog* presentationCatalog,
                                       uimodel::ListPresentations* listPresentations,
                                       ThemeCoordinator* themeCoordinator,
                                       i18n::MessageCatalog const& textCatalog,
                                       LayoutNode const& node)
        : _widget{runtime, textCatalog}
      {
        if (presentationCatalog != nullptr && listPresentations != nullptr)
        {
          _widget.setPresentationServices(presentationCatalog, listPresentations, themeCoordinator);
        }

        if (auto const it = node.props.find(kVariantProp); it != node.props.end())
        {
          if (auto const variant = it->second.asString(); variant == "title")
          {
            _widget.add_css_class("ao-variant-title");
          }
        }
      }

      Gtk::Widget& widget() override { return _widget; }

    private:
      TrackPresentationButton _widget;
    };
  } // namespace

  void registerTrackPresentationButtonComponent(ComponentRegistry& registry,
                                                rt::AppRuntime& runtime,
                                                uimodel::TrackPresentationCatalog* presentationCatalog,
                                                uimodel::ListPresentations* listPresentations,
                                                ThemeCoordinator* themeCoordinator,
                                                i18n::MessageCatalog const& textCatalog)
  {
    registry.registerSharedComponent(
      "track.presentationButton",
      [&runtime, presentationCatalog, listPresentations, themeCoordinator, textCatalog](
        LayoutBuildContext const& /*ctx*/, LayoutNode const& node)
      {
        return std::make_unique<TrackPresentationButtonComponent>(
          runtime, presentationCatalog, listPresentations, themeCoordinator, textCatalog, node);
      });
  }
} // namespace ao::gtk::layout
