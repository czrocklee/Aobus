/*
 * Copyright (C) 2024 RockStudio
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License along with
 * this program. If not see <http://www.gnu.org/licenses/>.
 */

#include "MainWindow.h"

#include <gtkmm.h>
#include <gtkmm/aboutdialog.h>

int main(int argc, char* argv[])
{
  Glib::set_application_name("RockStudio");

  auto app = Gtk::Application::create("com.rockstudio.app");

  // Add about action to application
  auto aboutAction = Gio::SimpleAction::create("about");
  aboutAction->signal_activate().connect([&app]([[maybe_unused]] const Glib::VariantBase& v) {
    Gtk::AboutDialog dialog;
    dialog.set_program_name("RockStudio");
    dialog.set_version("1.0");
    dialog.set_copyright("Copyright 2024 RockStudio");
    dialog.set_license_type(Gtk::License::LGPL_3_0);

    // Get active window to set as transient parent
    auto windows = app->get_windows();
    if (!windows.empty())
    {
      dialog.set_transient_for(*windows[0]);
    }

    dialog.present();
  });
  app->add_action(aboutAction);

  // Keep window alive - use shared_ptr
  Glib::RefPtr<MainWindow> mainWindow;

  // Connect to activate signal to create window after startup
  app->signal_activate().connect([&app, &mainWindow]() {
    mainWindow = Glib::make_refptr_for_instance<MainWindow>(new MainWindow());
    app->add_window(*mainWindow);
    mainWindow->present();
  });

  return app->run(argc, argv);
}
