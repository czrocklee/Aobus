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

#include "app/ui_TrackView.h"
#include <rs/core/MusicLibrary.h>
#include <QtCore/QThread>

#include <filesystem>
#include <optional>

class ImportWorker : public QThread
{
  Q_OBJECT

public:
  ImportWorker(rs::core::MusicLibrary& ml, const std::vector<std::filesystem::path>& files, QObject* parent = nullptr);

  void commit();

signals:
  void progressUpdated(QString path, int itemIndex);

  void workFinished();

protected:
  void run() override;

private:
  rs::core::MusicLibrary& _ml;
  std::optional<rs::core::LMDBWriteTransaction> _txn;
  const std::vector<std::filesystem::path>& _files;
};