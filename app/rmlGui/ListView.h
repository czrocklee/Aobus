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

#include <QtWidgets/QListWidget>
#include "app/rmlGui/ui_TrackView.h"
#include "TableModel.h"
#include <rs/ml/core/MusicLibrary.h>

class ListView : public QListWidget
{
  Q_OBJECT

public:
  ListView(TableModel::TrackList& list, QWidget* parent = nullptr);
  ~ListView() override;
  void reset(rs::ml::core::MusicLibrary& ml);

private:
  rs::ml::core::MusicLibrary* _ml;
};
