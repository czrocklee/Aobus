// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/library/presentation/TrackPresentationPickerViewModel.h>

#include <gtkmm/box.h>
#include <gtkmm/enums.h>
#include <gtkmm/menubutton.h>
#include <gtkmm/popover.h>
#include <sigc++/scoped_connection.h>

#include <memory>
#include <string_view>

namespace ao::rt
{
  class AppRuntime;
}
namespace ao::uimodel
{
  class TrackPresentationCatalog;
  class ListPresentationPreferenceStore;
}

namespace ao::gtk
{
  class ThemeCoordinator;

  /**
   * @brief TrackPresentationButton is a global menu button that manages view presentations for the focused track view.
   */
  class TrackPresentationButton final : public Gtk::Box
  {
  public:
    explicit TrackPresentationButton(rt::AppRuntime& runtime);
    ~TrackPresentationButton() override;

    TrackPresentationButton(TrackPresentationButton const&) = delete;
    TrackPresentationButton& operator=(TrackPresentationButton const&) = delete;
    TrackPresentationButton(TrackPresentationButton&&) = delete;
    TrackPresentationButton& operator=(TrackPresentationButton&&) = delete;

    void setPresentationServices(uimodel::TrackPresentationCatalog* catalog,
                                 uimodel::ListPresentationPreferenceStore* preferences,
                                 ThemeCoordinator* themeCoordinator);

  private:
    void render(uimodel::TrackPresentationPickerState const& state);
    void populatePresentationOptions(uimodel::TrackPresentationPickerState const& state);
    void handlePresentationSelected(std::string_view presentationId);
    void handleCreateCustomViewClicked();
    void showPresentationError(std::string_view message);

    rt::AppRuntime& _runtime;
    uimodel::TrackPresentationCatalog* _catalog = nullptr;
    std::unique_ptr<uimodel::TrackPresentationPickerViewModel> _viewModelPtr;
    ThemeCoordinator* _themeCoordinator = nullptr;
    uimodel::TrackPresentationPickerState _state;

    Gtk::MenuButton _button;
    Gtk::Popover _popover;
    Gtk::Box _menuBox{Gtk::Orientation::VERTICAL};
    sigc::scoped_connection _applyPresentationConn;
  };
} // namespace ao::gtk
