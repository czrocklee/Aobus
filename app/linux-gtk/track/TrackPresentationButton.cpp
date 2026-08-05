// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackPresentationButton.h"

#include "app/AppDialog.h"
#include "app/ThemeCoordinator.h"
#include "track/TrackCustomViewDialog.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/Log.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/uimodel/library/presentation/ListPresentationPreferenceStore.h>
#include <ao/uimodel/library/presentation/TrackPresentationCatalog.h>
#include <ao/uimodel/library/presentation/TrackPresentationPickerViewModel.h>

#include <glibmm/main.h>
#include <gtkmm/button.h>
#include <gtkmm/dialog.h>
#include <gtkmm/enums.h>
#include <gtkmm/object.h>
#include <gtkmm/separator.h>
#include <gtkmm/window.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace ao::gtk
{
  TrackPresentationButton::TrackPresentationButton(rt::AppRuntime& runtime)
    : _runtime{runtime}
  {
    set_valign(Gtk::Align::CENTER);

    _button.set_has_frame(false);
    _button.add_css_class("flat");
    _button.set_valign(Gtk::Align::CENTER);
    _button.add_css_class("ao-presentation-trigger");
    _popover.set_has_arrow(false);
    _popover.add_css_class("ao-presentation-popover");
    _popover.set_child(_menuBox);
    _button.set_popover(_popover);

    _menuBox.add_css_class("ao-presentation-menu-box");

    append(_button);
    render(_state);
  }

  TrackPresentationButton::~TrackPresentationButton() = default;

  void TrackPresentationButton::setPresentationServices(uimodel::TrackPresentationCatalog* catalog,
                                                        uimodel::ListPresentationPreferenceStore* preferences,
                                                        ThemeCoordinator* themeCoordinator)
  {
    // A pending apply belongs to the outgoing view model's session. Left
    // connected it would apply a stale selection to the runtime and then hand
    // the outgoing list id to whatever preference store is bound by then.
    _applyPresentationConn.disconnect();

    _catalog = catalog;
    _themeCoordinator = themeCoordinator;

    if (_catalog == nullptr || preferences == nullptr)
    {
      _viewModelPtr.reset();
      render({});
      return;
    }

    _viewModelPtr = std::make_unique<uimodel::TrackPresentationPickerViewModel>(
      _runtime.views(),
      _runtime.workspace(),
      *_catalog,
      *preferences,
      [this](uimodel::TrackPresentationPickerState const& state) { render(state); });
    _viewModelPtr->refresh();
  }

  void TrackPresentationButton::render(uimodel::TrackPresentationPickerState const& state)
  {
    _state = state;
    _button.set_sensitive(_state.enabled);
    _button.set_label(_state.label);
    populatePresentationOptions(_state);
  }

  void TrackPresentationButton::populatePresentationOptions(uimodel::TrackPresentationPickerState const& state)
  {
    auto* child = _menuBox.get_first_child();

    while (child != nullptr)
    {
      auto* const next = child->get_next_sibling();
      _menuBox.remove(*child);
      child = next;
    }

    for (auto const& item : state.menuItems)
    {
      if (item.type == uimodel::TrackPresentationMenuItemType::Separator)
      {
        auto* const sep = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
        sep->add_css_class("ao-presentation-menu-separator");
        _menuBox.append(*sep);
        continue;
      }

      auto* const btn = Gtk::make_managed<Gtk::Button>(item.label);
      btn->set_halign(Gtk::Align::FILL);
      btn->set_has_frame(false);
      btn->set_sensitive(item.enabled);

      if (!item.disabledReason.empty())
      {
        btn->set_tooltip_text(item.disabledReason);
      }

      btn->add_css_class("ao-presentation-menu-item");

      if (item.type == uimodel::TrackPresentationMenuItemType::CreateCustomView)
      {
        btn->signal_clicked().connect([this] { handleCreateCustomViewClicked(); });
      }
      else
      {
        auto const id = item.id;
        btn->signal_clicked().connect([this, id] { handlePresentationSelected(id); });
      }

      _menuBox.append(*btn);
    }
  }

  void TrackPresentationButton::handlePresentationSelected(std::string_view presentationId)
  {
    _popover.popdown();

    if (_viewModelPtr == nullptr)
    {
      return;
    }

    auto optSelection = _viewModelPtr->selectPresentation(presentationId);

    if (!optSelection)
    {
      return;
    }

    _applyPresentationConn.disconnect();
    _applyPresentationConn = Glib::signal_idle().connect(
      [this, selection = std::move(*optSelection)]
      {
        if (_runtime.workspace().snapshot().activeViewId != selection.targetViewId)
        {
          auto const message = std::string{"The selected track view is no longer active."};
          APP_LOG_ERROR("Failed to apply track presentation: {}", message);
          showPresentationError(message);
          return false;
        }

        if (auto const result = _runtime.workspace().setActivePresentation(selection.spec); !result)
        {
          APP_LOG_ERROR("Failed to apply track presentation: {}", result.error().message);
          showPresentationError(result.error().message);
          return false;
        }

        // Rebinding disconnects this idle callback before replacing the view
        // model, so the accepted selection belongs to the current session.
        _viewModelPtr->completeSelection(selection);

        return false;
      });
  }

  void TrackPresentationButton::showPresentationError(std::string_view message)
  {
    auto* const parentWindow = dynamic_cast<Gtk::Window*>(get_root());

    if (parentWindow == nullptr)
    {
      return;
    }

    auto* const dialog = AppDialog::presentMessage(
      *parentWindow,
      "Unable to Change Track View",
      std::string{message},
      {AppDialogAction{.label = "Close", .responseId = Gtk::ResponseType::CLOSE, .role = AppDialogActionRole::Cancel}},
      Gtk::ResponseType::CLOSE);

    if (_themeCoordinator != nullptr)
    {
      auto tokenPtr = std::make_shared<ThemeRegistrationToken>(_themeCoordinator->registerToplevel(*dialog));
      dialog->signal_hide().connect([tokenPtr] { (*tokenPtr).reset(); });
    }
  }

  void TrackPresentationButton::handleCreateCustomViewClicked()
  {
    _popover.popdown();

    if (!_state.enabled || _catalog == nullptr || _viewModelPtr == nullptr)
    {
      return;
    }

    auto* parentWindow = dynamic_cast<Gtk::Window*>(_button.get_root());

    if (parentWindow == nullptr)
    {
      return;
    }

    // _state is a cached render snapshot, so its view id can be stale by the
    // time the user clicks.
    auto const foundRes = _runtime.views().findTrackListState(_state.activeViewId);

    if (!foundRes)
    {
      return;
    }

    auto const& spec = foundRes->presentation;

    auto const label = std::string{_button.get_label()} + " Copy";
    auto dialog = TrackCustomViewDialog{*parentWindow, spec, label};
    auto optToken = std::optional<ThemeRegistrationToken>{};

    if (_themeCoordinator != nullptr)
    {
      optToken = _themeCoordinator->registerToplevel(dialog);
    }

    if (auto const optPreset = dialog.runDialog(); optPreset)
    {
      _catalog->addCustomPresentation(*optPreset);
      handlePresentationSelected(optPreset->spec.id);
    }
  }
} // namespace ao::gtk
