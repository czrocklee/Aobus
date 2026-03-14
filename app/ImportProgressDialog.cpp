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

#include "ImportProgressDialog.h"

#include <QtWidgets/QPushButton>

ImportProgressDialog::ImportProgressDialog(int maxItems, QWidget* parent, Qt::WindowFlags f) : QDialog{parent, f}
{
  setupUi(this);
  buttonBox->button(QDialogButtonBox::StandardButton::Ok)->setEnabled(false);
  progressBar->setRange(0, maxItems);
}

void ImportProgressDialog::onNewTrack(QString path, int itemIndex)
{
  currentFileLabel->setText(path);
  progressBar->setValue(itemIndex);
}

void ImportProgressDialog::ready() { buttonBox->button(QDialogButtonBox::StandardButton::Ok)->setEnabled(true); }
