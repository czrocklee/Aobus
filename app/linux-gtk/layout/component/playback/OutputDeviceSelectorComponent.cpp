// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "common/PopoverAttachment.h"
#include "layout/component/ComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include "playback/OutputDevicePopover.h"
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/playback/output/OutputDeviceIntent.h>
#include <ao/uimodel/playback/output/OutputDeviceViewModel.h>

#include <gtkmm/button.h>
#include <gtkmm/enums.h>
#include <gtkmm/label.h>
#include <gtkmm/widget.h>

#include <memory>
#include <utility>

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
      OutputDeviceSelectorComponent(rt::PlaybackService& playback,
                                    i18n::MessageCatalog textCatalog,
                                    uimodel::OutputDeviceIntent intent)
        : _playback{playback}
        , _textCatalog{std::move(textCatalog)}
        , _intent{std::move(intent)}
        // The button only names the active route; the popover it raises is what records a request.
        , _viewModel{_playback,
                     _textCatalog,
                     [this](uimodel::OutputDeviceViewState const& view)
                     {
                       _label.set_text(view.outputBackendSummary);
                       _button.set_tooltip_text(view.outputDeviceStatus);
                     },
                     uimodel::OutputDeviceIntent::discarded()}
      {
        _button.set_has_frame(false);
        _button.add_css_class("ao-output-device-selector-modern");
        _button.set_child(_label);

        _button.signal_clicked().connect(
          [this]
          {
            if (_popoverAttachment.hasPopover())
            {
              return;
            }

            auto popoverPtr =
              std::make_unique<OutputDevicePopover>(_playback, _textCatalog, _intent, Gtk::PositionType::TOP);
            _popoverAttachment.attach(std::move(popoverPtr), _button);
            _popoverAttachment.popup();
          });

        _viewModel.refresh();
      }

      Gtk::Widget& widget() override { return _button; }

    private:
      rt::PlaybackService& _playback;
      i18n::MessageCatalog _textCatalog;
      uimodel::OutputDeviceIntent _intent;
      Gtk::Button _button;
      Gtk::Label _label;
      uimodel::OutputDeviceViewModel _viewModel;
      PopoverAttachment _popoverAttachment;
    };
  } // namespace

  void registerOutputDeviceSelectorComponent(ComponentRegistry& registry,
                                             rt::PlaybackService& playback,
                                             i18n::MessageCatalog const& textCatalog,
                                             uimodel::OutputDeviceIntent const& outputDeviceIntent)
  {
    registry.registerSharedComponent(
      "playback.outputDeviceSelector",
      [&playback, textCatalog, intent = outputDeviceIntent](
        LayoutBuildContext const& /*ctx*/, LayoutNode const& /*node*/)
      { return std::make_unique<OutputDeviceSelectorComponent>(playback, textCatalog, intent); });
  }
} // namespace ao::gtk::layout
