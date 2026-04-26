// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "core/Log.h"
#include "platform/linux/ui/MainWindow.h"
#include "platform/linux/ui/TrackRowDataProvider.h"

#include <rs/AppVersion.h>

#include <gtkmm.h>
#include <gtkmm/aboutdialog.h>

#include <CLI/CLI.hpp>

int main(int argc, char* argv[])
{
  CLI::App cliApp{"RockStudio Music Library"};
  cliApp.allow_extras(); // Allow GTK specific arguments

  auto logLevel = app::core::LogLevel::Info;

  // Map strings to LogLevel enum for CLI11
  std::map<std::string, app::core::LogLevel> logMapping{{"trace", app::core::LogLevel::Trace},
                                                        {"debug", app::core::LogLevel::Debug},
                                                        {"info", app::core::LogLevel::Info},
                                                        {"warn", app::core::LogLevel::Warn},
                                                        {"error", app::core::LogLevel::Error},
                                                        {"critical", app::core::LogLevel::Critical},
                                                        {"off", app::core::LogLevel::Off}};

  int verbosity = 0;
  cliApp.add_flag("-v", verbosity, "Verbosity level (-v for debug, -vv for trace)");

  cliApp.add_option("--log-level", logLevel, "Set the logging level")
    ->transform(CLI::CheckedTransformer(logMapping, CLI::ignore_case));

  cliApp.add_flag_callback(
    "--version",
    []()
    {
      std::printf("RockStudio %s\n", rs::kAppVersion);
      std::exit(0);
    },
    "Show version information");

  try
  {
    cliApp.parse(argc, argv);
  }
  catch (CLI::ParseError const& e)
  {
    return cliApp.exit(e);
  }

  // Handle -v shortcuts if --log-level wasn't explicitly provided (or to override)
  if (cliApp.count("-v") > 0)
  {
    if (verbosity == 1)
      logLevel = app::core::LogLevel::Debug;
    else if (verbosity >= 2)
      logLevel = app::core::LogLevel::Trace;
  }

  app::core::Log::init(logLevel);
  APP_LOG_INFO("========================================================");
  APP_LOG_INFO("RockStudio {} starting...", rs::kAppVersion);

  Glib::set_application_name("RockStudio");

  auto app = Gtk::Application::create("com.rockstudio.app");

  // Add about action to application
  auto aboutAction = Gio::SimpleAction::create("about");
  aboutAction->signal_activate().connect(
    [&app]([[maybe_unused]] Glib::VariantBase const& /*variant*/)
    {
      auto dialog = Gtk::AboutDialog{};
      dialog.set_program_name("RockStudio");
      dialog.set_version(rs::kAppVersion);
      dialog.set_copyright("Copyright 2024 RockStudio");
      dialog.set_license_type(Gtk::License::LGPL_3_0);

      // Get active window to set as transient parent
      if (auto windows = app->get_windows(); !windows.empty())
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
  auto mainWindow = Glib::RefPtr<app::ui::MainWindow>{};

  // Connect to activate signal to create window after startup
  app->signal_activate().connect(
    [&app, &mainWindow]()
    {
      mainWindow = Glib::make_refptr_for_instance<app::ui::MainWindow>(new app::ui::MainWindow());
      app->add_window(*mainWindow);
      mainWindow->present();
    });

  auto remainingArgs = cliApp.remaining_for_passthrough();
  remainingArgs.insert(remainingArgs.begin(), argv[0]);

  std::vector<char*> gtkArgv;
  gtkArgv.reserve(remainingArgs.size());
  for (auto& arg : remainingArgs)
  {
    gtkArgv.push_back(arg.data());
  }
  int gtkArgc = static_cast<int>(gtkArgv.size());

  auto const result = app->run(gtkArgc, gtkArgv.data());
  app::core::Log::shutdown();
  return result;
}
