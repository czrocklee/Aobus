// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "core/Log.h"
#include "platform/linux/ui/MainWindow.h"

#include <gtkmm.h>
#include <gtkmm/aboutdialog.h>

int main(int argc, char* argv[])
{
  app::core::Log::init();
  APP_LOG_INFO("RockStudio starting...");

  Glib::set_application_name("RockStudio");

  auto app = Gtk::Application::create("com.rockstudio.app");

  // Add about action to application
  auto aboutAction = Gio::SimpleAction::create("about");
  aboutAction->signal_activate().connect(
    [&app]([[maybe_unused]] Glib::VariantBase const& /*variant*/)
    {
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

  // Add quit action
  auto quitAction = Gio::SimpleAction::create("quit");
  quitAction->signal_activate().connect([&app]([[maybe_unused]] Glib::VariantBase const& /*variant*/) { app->quit(); });
  app->add_action(quitAction);

  // Keep window alive - use shared_ptr
  Glib::RefPtr<app::ui::MainWindow> mainWindow;

  // Connect to activate signal to create window after startup
  app->signal_activate().connect(
    [&app, &mainWindow]()
    {
      mainWindow = Glib::make_refptr_for_instance<app::ui::MainWindow>(new app::ui::MainWindow());
      app->add_window(*mainWindow);
      mainWindow->present();
    });

  auto const result = app->run(argc, argv);
  app::core::Log::shutdown();
  return result;
}
