// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <gtkmm.h>

namespace app::ui
{

  class ImportProgressDialog final : public Gtk::Dialog
  {
  public:
    ImportProgressDialog(std::int32_t maxItems, Gtk::Window& parent);
    virtual ~ImportProgressDialog();

    void onNewTrack(std::string const& path, std::int32_t itemIndex);
    void ready();

  private:
    void setupUi(std::int32_t maxItems);

    Gtk::Label _progressLabel;
    Gtk::ProgressBar _progressBar;
    Gtk::Button _okButton;
    std::int32_t _maxItems;
  };

} // namespace app::ui
