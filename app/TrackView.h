// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "TableModel.h"
#include "app/ui_TrackView.h"

class TrackView
  : public QWidget
  , public Ui_TrackView
{
  Q_OBJECT

public:
  TrackView(TableModel::AbstractTrackList& list, QWidget* parent = nullptr);
  ~TrackView() override;
};
