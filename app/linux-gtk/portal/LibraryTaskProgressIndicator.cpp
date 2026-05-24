// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "portal/LibraryTaskProgressIndicator.h"

#include "layout/LayoutConstants.h"
#include <ao/rt/LibraryMutationService.h>

#include <gtkmm/box.h>
#include <gtkmm/enums.h>
#include <pangomm/layout.h>

#include <cstddef>

namespace ao::gtk::portal
{
  namespace
  {
    constexpr int kDefaultLabelMaxWidth = 30;
    constexpr int kDefaultProgressBarWidth = 150;
  }

  LibraryTaskProgressIndicator::LibraryTaskProgressIndicator(rt::LibraryMutationService& mutationService)
    : Gtk::Box{Gtk::Orientation::HORIZONTAL, layout::kSpacingSmall}, _mutationService{mutationService}
  {
    setupUi();

    _progressSub = _mutationService.onLibraryTaskProgress(
      [this](rt::LibraryMutationService::LibraryTaskProgressUpdated const& ev)
      {
        set_visible(true);
        _label.set_text(ev.message);
        _progressBar.set_fraction(ev.fraction);
      });

    _completedSub = _mutationService.onLibraryTaskCompleted([this](std::size_t /*count*/) { set_visible(false); });

    set_visible(false);
  }

  LibraryTaskProgressIndicator::~LibraryTaskProgressIndicator() = default;

  void LibraryTaskProgressIndicator::setupUi()
  {
    _label.set_halign(Gtk::Align::START);
    _label.set_ellipsize(Pango::EllipsizeMode::END);
    _label.set_max_width_chars(kDefaultLabelMaxWidth);
    append(_label);

    _progressBar.set_valign(Gtk::Align::CENTER);
    _progressBar.set_size_request(kDefaultProgressBarWidth, -1);
    append(_progressBar);
  }
} // namespace ao::gtk::portal
