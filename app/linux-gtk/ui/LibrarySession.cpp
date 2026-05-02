// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "LibrarySession.h"

namespace ao::gtk
{
  std::unique_ptr<LibrarySession> makeLibrarySession(std::filesystem::path const& rootPath)
  {
    auto session = std::make_unique<LibrarySession>();

    session->musicLibrary = std::make_unique<ao::library::MusicLibrary>(rootPath.string());

    session->rowDataProvider = std::make_unique<TrackRowDataProvider>(*session->musicLibrary);
    session->rowDataProvider->loadAll();

    session->allTrackIds = std::make_unique<ao::model::AllTrackIdsList>(session->musicLibrary->tracks());
    session->smartListEngine = std::make_unique<ao::model::SmartListEngine>(*session->musicLibrary);

    auto txn = session->musicLibrary->readTransaction();
    session->allTrackIds->reloadFromStore(txn);

    return session;
  }
} // namespace ao::gtk
