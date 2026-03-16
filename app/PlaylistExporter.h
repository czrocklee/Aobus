// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

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

  void onEndInsert(TrackId, rs::fbs::TrackT const&, AbstractTrackList::Index) override { scheduleForWrite(); }

  void onEndUpdate(TrackId, rs::fbs::TrackT const&, AbstractTrackList::Index) override { scheduleForWrite(); }

  void onEndRemove(TrackId, AbstractTrackList::Index) override { scheduleForWrite(); }

  void onEndClear() override { scheduleForWrite(); }

  AbstractTrackList& _list;
  std::filesystem::path const _root;
  std::filesystem::path const _path;
  std::string const _name;
  QTimer* _timer;
};