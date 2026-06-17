// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackPresentationButton.h"

#include "app/ThemeCoordinator.h"
#include "track/TrackCustomViewDialog.h"
#include <ao/Type.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/StateTypes.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/uimodel/track/TrackPresentationViewModel.h>

#include <glibmm/main.h>
#include <gtkmm/button.h>
#include <gtkmm/enums.h>
#include <gtkmm/object.h>
#include <gtkmm/separator.h>
#include <gtkmm/window.h>

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace ao::gtk
{
  TrackPresentationButton::TrackPresentationButton(rt::AppRuntime& runtime)
    : _runtime{runtime}
  {
    _button.set_has_frame(true);
    _popover.set_has_arrow(false);
    _popover.set_child(_menuBox);
    _button.set_popover(_popover);

    _menuBox.add_css_class("ao-presentation-menu-box");

    append(_button);

    _focusSub = _runtime.workspace().onFocusedViewChanged([this](rt::ViewId viewId) { onFocusedViewChanged(viewId); });

    onFocusedViewChanged(_runtime.workspace().layoutState().activeViewId);
  }

  TrackPresentationButton::~TrackPresentationButton() = default;

  void TrackPresentationButton::setPresentationStore(uimodel::track::TrackPresentationViewModel* store,
                                                     ThemeCoordinator* themeController)
  {
    _presentationStore = store;
    _themeController = themeController;
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

    auto label = _presentationStore != nullptr ? _presentationStore->labelForId(pres.id) : std::string{pres.id};

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

    for (auto const& item : _presentationStore->menuItems())
    {
      if (item.type == uimodel::track::TrackPresentationMenuItemType::Separator)
      {
        auto* const sep = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
        sep->add_css_class("ao-presentation-menu-separator");
        _menuBox.append(*sep);
        continue;
      }

      auto* const btn = Gtk::make_managed<Gtk::Button>(item.label);
      btn->set_halign(Gtk::Align::FILL);
      btn->set_has_frame(false);
      btn->add_css_class("ao-presentation-menu-item");

      if (item.type == uimodel::track::TrackPresentationMenuItemType::CreateCustomView)
      {
        btn->signal_clicked().connect([this] { onCreateCustomViewClicked(); });
      }
      else
      {
        auto const id = item.id;
        btn->signal_clicked().connect([this, id] { onPresentationSelected(id); });
      }

      _menuBox.append(*btn);
    }
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

    _button.set_label(_presentationStore->labelForId(presentationId));

    auto spec = rt::TrackPresentationSpec{*optSpec};

    auto const state = _runtime.views().trackListState(_activeViewId);

    if (state.listId != ao::kInvalidListId)
    {
      _presentationStore->setPresentationIdForList(state.listId, spec.id);
    }

    Glib::signal_idle().connect_once([this, spec = std::move(spec)]
                                     { _runtime.workspace().setActivePresentation(spec); });
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
    auto const& spec = state.presentation;

    auto const label = std::string{_button.get_label()} + " Copy";
    auto dialog = TrackCustomViewDialog{*parentWindow, spec, label};
    // NOLINTNEXTLINE(aobus-readability-use-if-init-statement) — RAII: must outlive if-block to keep dialog themed
    auto optToken = std::optional<ThemeRegistrationToken>{};

    if (_themeController != nullptr)
    {
      optToken = _themeController->registerToplevel(dialog);
    }

    if (auto const optResult = dialog.runDialog(); optResult)
    {
      if (optResult->deleted)
      {
        _presentationStore->removeCustomPresentation(optResult->state.spec.id);

        if (spec.id == optResult->state.spec.id)
        {
          onPresentationSelected(rt::kDefaultTrackPresentationId);
        }
      }
      else
      {
        _presentationStore->addCustomPresentation(optResult->state);
        onPresentationSelected(optResult->state.spec.id);
      }

      populatePresentationOptions();
    }
  }
} // namespace ao::gtk
