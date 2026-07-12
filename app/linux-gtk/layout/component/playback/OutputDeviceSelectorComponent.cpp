// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "PlaybackComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include "playback/OutputDevicePopover.h"
#include <ao/rt/AppRuntime.h>
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/playback/output/OutputDeviceViewModel.h>

#include <gtkmm/button.h>
#include <gtkmm/enums.h>
#include <gtkmm/label.h>
#include <gtkmm/object.h>
#include <gtkmm/widget.h>

#include <memory>

namespace ao::gtk::layout
{
  using namespace uimodel;
  namespace
  {
    /**
     * @brief playback.outputDeviceSelector
     */
    class OutputDeviceSelectorComponent final : public LayoutComponent
    {
    public:
      OutputDeviceSelectorComponent(LayoutBuildContext& ctx, LayoutNode const& /*node*/)
        : _playback{ctx.runtime.playback()}
        , _viewModel{_playback,
                     [this](uimodel::OutputDeviceViewState const& view)
                     {
                       _label.set_text(view.outputBackendSummary);
                       _button.set_tooltip_text(view.outputDeviceStatus);
                     }}
      {
        _button.set_has_frame(false);
        _button.add_css_class("ao-output-device-selector-modern");
        _button.set_child(_label);

        _button.signal_clicked().connect(
          [this]
          {
            auto* const popover = Gtk::make_managed<OutputDevicePopover>(_playback, Gtk::PositionType::TOP);
            popover->set_parent(_button);
            popover->signal_closed().connect([popover] { popover->unparent(); });
            popover->popup();
          });

        _viewModel.refresh();
      }

      Gtk::Widget& widget() override { return _button; }

    private:
      rt::PlaybackService& _playback;
      Gtk::Button _button;
      Gtk::Label _label;
      uimodel::OutputDeviceViewModel _viewModel;
    };

    std::unique_ptr<LayoutComponent> createOutputDeviceSelector(LayoutBuildContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<OutputDeviceSelectorComponent>(ctx, node);
    }
  } // namespace

  void registerOutputDeviceSelectorComponent(ComponentRegistry& registry)
  {
    registry.registerComponent({.type = "playback.outputDeviceSelector",
                                .displayName = "Output Device Selector",
                                .category = LayoutComponentCategory::Playback,
                                .minChildren = 0,
                                .optMaxChildren = 0},
                               createOutputDeviceSelector);
  }
} // namespace ao::gtk::layout
