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

#pragma once

#include "app/ui_ImportProgressDialog.h"
#include <rs/fbs/Track_generated.h>

class ImportProgressDialog : public QDialog, public Ui::ImportProgressDialog
{
  Q_OBJECT

public:
  ImportProgressDialog(int maxItems, QWidget* parent = nullptr, Qt::WindowFlags f = Qt::WindowFlags());

public slots:
  void onNewTrack(QString path, int itemIndex);

  void ready();
};
