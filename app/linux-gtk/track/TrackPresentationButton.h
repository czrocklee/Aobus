// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/track/TrackPresentationWorkflow.h>

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
namespace ao::uimodel::track
{
  class TrackPresentationCatalog;
  class TrackPresentationPreferenceStore;
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

    void setPresentationServices(uimodel::track::TrackPresentationCatalog* catalog,
                                 uimodel::track::TrackPresentationPreferenceStore* preferences,
                                 ThemeCoordinator* themeController);

  private:
    void render(uimodel::track::TrackPresentationPickerState const& state);
    void populatePresentationOptions(uimodel::track::TrackPresentationPickerState const& state);
    void onPresentationSelected(std::string_view presentationId);
    void onCreateCustomViewClicked();
    void applyCommand(uimodel::track::TrackPresentationApplyCommand const& command);

    rt::AppRuntime& _runtime;
    uimodel::track::TrackPresentationCatalog* _catalog = nullptr;
    std::unique_ptr<uimodel::track::TrackPresentationWorkflow> _workflowPtr;
    ThemeCoordinator* _themeController = nullptr;
    uimodel::track::TrackPresentationPickerState _state;

    Gtk::MenuButton _button;
    Gtk::Popover _popover;
    Gtk::Box _menuBox{Gtk::Orientation::VERTICAL};
    sigc::scoped_connection _applyPresentationConn;
  };
} // namespace ao::gtk
