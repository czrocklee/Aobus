// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "app/ui_AddTrackDialog.h"
#include <rs/fbs/Track_generated.h>

class AddTrackDialog
  : public QDialog
  , public Ui::AddTrackDialog
{
  Q_OBJECT

public:
  AddTrackDialog(QWidget* parent = nullptr, Qt::WindowFlags f = Qt::WindowFlags());

  rs::fbs::TrackT track() const;
};
