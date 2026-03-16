// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

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
