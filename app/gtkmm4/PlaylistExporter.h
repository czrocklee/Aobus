// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "model/TrackIdList.h"
#include "model/TrackRowDataProvider.h"

#include <rs/core/MusicLibrary.h>

#include <filesystem>
#include <memory>
#include <sigc++/sigc++.h>

class PlaylistExporter : public app::gtkmm4::model::TrackIdListObserver
{
public:
  using TrackId = rs::core::TrackId;

  PlaylistExporter(app::gtkmm4::model::TrackIdList& list,
                  app::gtkmm4::model::TrackRowDataProvider& provider,
                  std::filesystem::path root,
                  std::filesystem::path path);
  ~PlaylistExporter() override;

  void triggerWrite();

  // TrackIdListObserver interface
  void onReset() override;
  void onInserted(TrackId id, std::size_t index) override;
  void onUpdated(TrackId id, std::size_t index) override;
  void onRemoved(TrackId id, std::size_t index) override;

private:
  void writeFile();
  void scheduleForWrite();

  app::gtkmm4::model::TrackIdList& _list;
  app::gtkmm4::model::TrackRowDataProvider& _provider;
  std::filesystem::path const _root;
  std::filesystem::path const _path;
  std::unique_ptr<sigc::connection> _timeoutConnection;
};