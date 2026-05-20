// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "runtime/CorePrimitives.h"

#include <gtkmm/box.h>
#include <gtkmm/enums.h>
#include <gtkmm/menubutton.h>
#include <gtkmm/popover.h>

#include <string_view>

namespace ao::rt
{
  class AppRuntime;
}

namespace ao::gtk
{
  class TrackPresentationStore;

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

    void setPresentationStore(TrackPresentationStore* store);

  private:
    void onFocusedViewChanged(rt::ViewId viewId);
    void populatePresentationOptions();
    void onPresentationSelected(std::string_view presentationId);
    void onCreateCustomViewClicked();

    rt::AppRuntime& _runtime;
    rt::ViewId _activeViewId = rt::kInvalidViewId;
    TrackPresentationStore* _presentationStore = nullptr;

    Gtk::MenuButton _button;
    Gtk::Popover _popover;
    Gtk::Box _menuBox{Gtk::Orientation::VERTICAL};

    rt::Subscription _focusSub;
  };
} // namespace ao::gtk
