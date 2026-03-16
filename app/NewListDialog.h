// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "app/ui_NewListDialog.h"
#include <rs/fbs/List_generated.h>

class NewListDialog
  : public QDialog
  , public Ui::NewListDialog
{
  Q_OBJECT

public:
  NewListDialog(QWidget* parent = nullptr, Qt::WindowFlags f = Qt::WindowFlags());

  rs::fbs::ListT list() const;
};
