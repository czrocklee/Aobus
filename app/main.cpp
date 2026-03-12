/*
 * Copyright (C) <year> <name of author>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of  MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

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
  const auto styleOverride = qgetenv("QT_STYLE_OVERRIDE").toLower();

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
