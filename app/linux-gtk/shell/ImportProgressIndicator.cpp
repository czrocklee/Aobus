// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "shell/ImportProgressIndicator.h"
#include "layout/LayoutConstants.h"
#include <runtime/AppSession.h>
#include <runtime/LibraryMutationService.h>

namespace ao::gtk
{
  ImportProgressIndicator::ImportProgressIndicator(ao::rt::AppSession& session)
    : _session{session}
  {
    _container.set_spacing(Layout::kSpacingXLarge);
    _container.set_visible(false);

    _label.add_css_class("dim-label");
    _container.append(_label);

    _progressBar.set_valign(Gtk::Align::CENTER);
    _progressBar.set_size_request(kProgressBarWidth, -1);
    _container.append(_progressBar);

    _progressSub = _session.mutation().onImportProgress(
      [this](auto const& ev)
      {
        _container.set_visible(true);
        _progressBar.set_fraction(ev.fraction);
        _label.set_text(ev.message);
      });

    _completedSub = _session.mutation().onImportCompleted([this](auto) { _container.set_visible(false); });
  }

  ImportProgressIndicator::~ImportProgressIndicator() = default;
} // namespace ao::gtk
