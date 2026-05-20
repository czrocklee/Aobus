// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackPresentationButton.h"

#include "app/UIState.h"
#include "runtime/AppRuntime.h"
#include "runtime/CorePrimitives.h"
#include "runtime/StateTypes.h"
#include "runtime/TrackPresentationPreset.h"
#include "runtime/ViewService.h"
#include "runtime/WorkspaceService.h"
#include "track/TrackCustomViewDialog.h"
#include "track/TrackPresentationStore.h"

#include <glibmm/main.h>
#include <gtkmm/button.h>
#include <gtkmm/enums.h>
#include <gtkmm/object.h>
#include <gtkmm/separator.h>
#include <gtkmm/window.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::gtk
{
  TrackPresentationButton::TrackPresentationButton(rt::AppRuntime& runtime)
    : _runtime{runtime}
  {
    _button.set_has_frame(true);
    _popover.set_has_arrow(false);
    _popover.set_child(_menuBox);
    _button.set_popover(_popover);

    append(_button);

    _focusSub = _runtime.workspace().onFocusedViewChanged([this](rt::ViewId viewId) { onFocusedViewChanged(viewId); });

    onFocusedViewChanged(_runtime.workspace().layoutState().activeViewId);
  }

  TrackPresentationButton::~TrackPresentationButton() = default;

  void TrackPresentationButton::setPresentationStore(TrackPresentationStore* store)
  {
    _presentationStore = store;
    populatePresentationOptions();
  }

  void TrackPresentationButton::onFocusedViewChanged(rt::ViewId viewId)
  {
    _activeViewId = viewId;

    if (_activeViewId == rt::kInvalidViewId)
    {
      _button.set_sensitive(false);
      _button.set_label("Presentation");
      return;
    }

    _button.set_sensitive(true);
    auto const state = _runtime.views().trackListState(_activeViewId);
    auto const& pres = state.presentation;

    auto label = pres.presentationId;

    if (auto const* builtin = rt::builtinTrackPresentationPreset(pres.presentationId))
    {
      label = std::string(builtin->label);
    }
    else if (_presentationStore != nullptr)
    {
      auto const& customs = _presentationStore->customPresentations();
      auto const it = std::ranges::find(customs, pres.presentationId, &CustomTrackPresentationState::id);

      if (it != customs.end())
      {
        label = it->label;
      }
    }

    _button.set_label(label);
    populatePresentationOptions();
  }

  void TrackPresentationButton::populatePresentationOptions()
  {
    auto* child = _menuBox.get_first_child();

    while (child != nullptr)
    {
      auto* const next = child->get_next_sibling();
      _menuBox.remove(*child);
      child = next;
    }

    if (_presentationStore == nullptr)
    {
      return;
    }

    auto const builtins = _presentationStore->builtinPresets();

    for (auto const& preset : builtins)
    {
      auto* const btn = Gtk::make_managed<Gtk::Button>(std::string(preset.label));
      btn->set_halign(Gtk::Align::FILL);
      btn->set_has_frame(false);
      btn->get_style_context()->add_class("flat");

      auto const id = std::string(preset.spec.id);
      btn->signal_clicked().connect([this, id] { onPresentationSelected(id); });

      _menuBox.append(*btn);
    }

    auto const& customs = _presentationStore->customPresentations();

    if (!customs.empty())
    {
      auto* const sep = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
      sep->add_css_class("ao-presentation-menu-separator");
      _menuBox.append(*sep);

      for (auto const& custom : customs)
      {
        auto* const btn = Gtk::make_managed<Gtk::Button>(custom.label);
        btn->set_halign(Gtk::Align::FILL);
        btn->set_has_frame(false);
        btn->get_style_context()->add_class("flat");

        auto const id = custom.id;
        btn->signal_clicked().connect([this, id] { onPresentationSelected(id); });

        _menuBox.append(*btn);
      }
    }

    auto* const finalSep = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
    finalSep->add_css_class("ao-presentation-menu-separator");
    _menuBox.append(*finalSep);

    auto* const createBtn = Gtk::make_managed<Gtk::Button>("Create Custom View...");
    createBtn->set_halign(Gtk::Align::FILL);
    createBtn->set_has_frame(false);
    createBtn->get_style_context()->add_class("flat");
    createBtn->signal_clicked().connect([this] { onCreateCustomViewClicked(); });
    _menuBox.append(*createBtn);
  }

  void TrackPresentationButton::onPresentationSelected(std::string_view presentationId)
  {
    _popover.popdown();

    if (_activeViewId == rt::kInvalidViewId || _presentationStore == nullptr)
    {
      return;
    }

    auto const optSpec = _presentationStore->specForId(presentationId);

    if (!optSpec)
    {
      return;
    }

    auto label = std::string(presentationId);

    if (auto const* builtin = rt::builtinTrackPresentationPreset(presentationId))
    {
      label = std::string(builtin->label);
    }
    else
    {
      auto const& customs = _presentationStore->customPresentations();
      auto const it = std::ranges::find(customs, presentationId, &CustomTrackPresentationState::id);

      if (it != customs.end())
      {
        label = it->label;
      }
    }

    _button.set_label(label);

    auto spec = rt::TrackPresentationSpec{*optSpec};
    Glib::signal_idle().connect_once(
      [this, viewId = _activeViewId, spec = std::move(spec)]
      {
        if (viewId != rt::kInvalidViewId)
        {
          _runtime.views().setPresentation(viewId, spec);
        }
      });
  }

  void TrackPresentationButton::onCreateCustomViewClicked()
  {
    _popover.popdown();

    if (_activeViewId == rt::kInvalidViewId || _presentationStore == nullptr)
    {
      return;
    }

    auto* parentWindow = dynamic_cast<Gtk::Window*>(_button.get_root());

    if (parentWindow == nullptr)
    {
      return;
    }

    auto const state = _runtime.views().trackListState(_activeViewId);
    auto const spec = rt::presentationSpecFromState(state.presentation);

    auto const label = std::string(_button.get_label()) + " Copy";
    auto dialog = TrackCustomViewDialog{*parentWindow, spec, label};

    if (auto const optResult = dialog.runDialog())
    {
      if (optResult->deleted)
      {
        _presentationStore->removeCustomPresentation(optResult->state.id);

        if (spec.id == optResult->state.id)
        {
          onPresentationSelected(rt::kDefaultTrackPresentationId);
        }
      }
      else
      {
        _presentationStore->addCustomPresentation(optResult->state);
        onPresentationSelected(optResult->state.id);
      }

      populatePresentationOptions();
    }
  }
} // namespace ao::gtk
