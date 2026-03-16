// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "app/ui_ImportProgressDialog.h"
#include <rs/fbs/Track_generated.h>

class ImportProgressDialog
  : public QDialog
  , public Ui::ImportProgressDialog
{
  Q_OBJECT

public:
  ImportProgressDialog(int maxItems, QWidget* parent = nullptr, Qt::WindowFlags f = Qt::WindowFlags());

public slots:
  void onNewTrack(QString path, int itemIndex);

  void ready();
};
