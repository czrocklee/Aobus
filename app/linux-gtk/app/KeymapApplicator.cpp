// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/KeymapApplicator.h"

#include "app/GtkAccelTranslator.h"
#include <ao/uimodel/input/KeymapModel.h>
#include <ao/utility/Log.h>

#include <glibmm/ustring.h>
#include <gtkmm/application.h>

#include <string>
#include <vector>

namespace ao::gtk
{
  void applyKeymapAccelerators(Gtk::Application& app, uimodel::input::KeymapModel const& keymap)
  {
    // set_accels_for_action accumulates state on the application, so reconcile before applying:
    // clear any "win.*" accelerator the current keymap no longer mentions. This matters when an
    // action is reset and has no shipped default (resetToDefault erases the entry entirely), which
    // would otherwise leave the stale accelerator firing until restart. Every "win.*" accelerator
    // is owned by this function, so clearing the unmentioned ones is safe.
    for (auto const& detailedName : app.list_action_descriptions())
    {
      if (auto const& raw = detailedName.raw(); raw.starts_with("win.") && !keymap.bindings().contains(raw.substr(4)))
      {
        app.set_accels_for_action(detailedName, {});
      }
    }

    for (auto const& [actionId, chords] : keymap.bindings())
    {
      auto accels = std::vector<Glib::ustring>{};

      for (auto const& chord : chords)
      {
        if (auto const optAccel = toGtkAccel(chord); optAccel)
        {
          accels.emplace_back(*optAccel);
        }
        else
        {
          APP_LOG_WARN("applyKeymapAccelerators: unmappable chord '{}' for action '{}'", chord.toString(), actionId);
        }
      }

      app.set_accels_for_action("win." + actionId, accels);
    }
  }
}
