// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "NewListDialog.h"

NewListDialog::NewListDialog(QWidget* parent, Qt::WindowFlags f) : QDialog{parent, f} { setupUi(this); }

rs::fbs::ListT NewListDialog::list() const
{
  rs::fbs::ListT l;
  l.name = lineEditName->text().toStdString();
  l.desc = lineEditDesc->text().toStdString();
  l.expr = lineEditExpr->text().toStdString();
  return l;
}
