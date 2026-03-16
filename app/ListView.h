// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "TableModel.h"
#include "app/ui_TrackView.h"
#include <QtWidgets/QListWidget>
#include <rs/core/MusicLibrary.h>

class ListView : public QListWidget
{
  Q_OBJECT

public:
  ListView(TableModel::TrackList& list, QWidget* parent = nullptr);
  ~ListView() override;
  void reset(rs::core::MusicLibrary& ml);

private:
  rs::core::MusicLibrary* _ml;
};
