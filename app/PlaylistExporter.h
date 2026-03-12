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

#include "Common.h"
#include <QtCore/QTimer>

#include <filesystem>
#include <optional>

class PlaylistExporter
  : public QObject
  , public TrackObserver
{
  Q_OBJECT;

public:
  PlaylistExporter(AbstractTrackList& list, std::filesystem::path root, std::filesystem::path path, QObject* parent);

private slots:
  void writeFile();

private:
  void scheduleForWrite();

  void onEndInsert(TrackId, const rs::fbs::TrackT&, AbstractTrackList::Index) override { scheduleForWrite(); }

  void onEndUpdate(TrackId, const rs::fbs::TrackT&, AbstractTrackList::Index) override { scheduleForWrite(); }

  void onEndRemove(TrackId, AbstractTrackList::Index) override { scheduleForWrite(); }

  void onEndClear() override { scheduleForWrite(); }

  AbstractTrackList& _list;
  const std::filesystem::path _root;
  const std::filesystem::path _path;
  const std::string _name;
  QTimer* _timer;
};