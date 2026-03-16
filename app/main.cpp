// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "MainWindow.h"
#include <QtWidgets/QApplication>

/*#ifndef _WIN32
#include <QtPlugin>
Q_IMPORT_PLUGIN(QXcbIntegrationPlugin)
#endif*/

int main(int argc, char* argv[])
{
  QCoreApplication::setOrganizationName("RockStudio");
  QCoreApplication::setApplicationName("RockStudio");
  QCoreApplication::setOrganizationDomain("rs.com");

#ifdef _WIN32
  QApplication::setStyle("fusion");
#elif defined(__linux__)
  const auto platformTheme = qgetenv("QT_QPA_PLATFORMTHEME").toLower();
  auto const styleOverride = qgetenv("QT_STYLE_OVERRIDE").toLower();

  if (platformTheme.contains("qt5ct") || platformTheme.contains("qt6ct"))
  {
    qunsetenv("QT_QPA_PLATFORMTHEME");
  }

  if (styleOverride.contains("qt6ct") || styleOverride.contains("kvantum"))
  {
    qunsetenv("QT_STYLE_OVERRIDE");
    QApplication::setStyle("fusion");
  }
  else if (!styleOverride.isEmpty())
  {
    QApplication::setStyle(styleOverride);
  }
#endif

  QApplication app(argc, argv);
  MainWindow mw;
  mw.show();
  return app.exec();
}
