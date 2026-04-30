// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 RockStudio Contributors

#include "platform/linux/ui/LibrarySession.h"

namespace app::ui
{
  std::unique_ptr<LibrarySession> makeLibrarySession(std::filesystem::path const& rootPath)
  {
    auto session = std::make_unique<LibrarySession>();

    session->musicLibrary = std::make_unique<rs::library::MusicLibrary>(rootPath.string());
    
    session->rowDataProvider = std::make_unique<TrackRowDataProvider>(*session->musicLibrary);
    session->rowDataProvider->loadAll();
    
    session->allTrackIds = std::make_unique<rs::model::AllTrackIdsList>(session->musicLibrary->tracks());
    session->smartListEngine = std::make_unique<rs::model::SmartListEngine>(*session->musicLibrary);

    auto txn = session->musicLibrary->readTransaction();
    session->allTrackIds->reloadFromStore(txn);

    return session;
  }
} // namespace app::ui
