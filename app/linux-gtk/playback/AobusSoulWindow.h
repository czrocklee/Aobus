// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "playback/AobusSoul.h"
#include <gtkmm/window.h>

namespace ao::gtk
{
  /**
   * @class AobusSoulWindow
   * @brief A fullscreen overlay that displays a giant Aobus Soul.
   */
  class AobusSoulWindow final : public Gtk::Window
  {
  public:
    AobusSoulWindow();
    ~AobusSoulWindow() override;

    AobusSoulWindow(AobusSoulWindow const&) = delete;
    AobusSoulWindow& operator=(AobusSoulWindow const&) = delete;
    AobusSoulWindow(AobusSoulWindow&&) = delete;
    AobusSoulWindow& operator=(AobusSoulWindow&&) = delete;

    /**
     * @brief Bind the window to an AppSession.
     * @param session The AppSession to bind to.
     */
    void bind(ao::rt::AppSession& session);

  protected:
    void on_show() override;
    void on_hide() override;

  private:
    void ensureCss();

    ao::rt::AppSession* _session = nullptr;
    AobusSoul _bigSoul{};
  };
} // namespace ao::gtk
