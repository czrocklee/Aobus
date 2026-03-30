// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/core/MusicLibrary.h>

#include "model/TrackIdList.h"
#include "model/TrackRowDataProvider.h"

#include <sigc++/sigc++.h>

#include <filesystem>
#include <memory>

class PlaylistExporter final : public app::model::TrackIdListObserver
{
public:
  using TrackId = rs::core::TrackId;

  PlaylistExporter(app::model::TrackIdList& list,
                   app::model::TrackRowDataProvider& provider,
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

  app::model::TrackIdList& _list;
  app::model::TrackRowDataProvider& _provider;
  std::filesystem::path const _root;
  std::filesystem::path const _path;
  std::unique_ptr<sigc::connection> _timeoutConnection;
};