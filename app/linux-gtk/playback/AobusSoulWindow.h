// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "app/AobusSoul.h"
#include <ao/uimodel/playback/soul/AobusSoulViewModel.h>

#include <gtkmm/window.h>

#include <memory>

namespace ao::rt
{
  class PlaybackService;
}

namespace ao::gtk
{
  class AobusSoulWindow final : public Gtk::Window
  {
  public:
    AobusSoulWindow();
    ~AobusSoulWindow() override;

    AobusSoulWindow(AobusSoulWindow const&) = delete;
    AobusSoulWindow& operator=(AobusSoulWindow const&) = delete;
    AobusSoulWindow(AobusSoulWindow&&) = delete;
    AobusSoulWindow& operator=(AobusSoulWindow&&) = delete;

    void bind(rt::PlaybackService& playback);

  protected:
    void on_show() override;
    void on_hide() override;

  private:
    rt::PlaybackService* _playback = nullptr;
    AobusSoul _bigSoul{};
    std::unique_ptr<uimodel::AobusSoulViewModel> _controllerPtr;
  };
} // namespace ao::gtk
