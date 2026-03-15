#pragma once

#include <rs/core/MusicLibrary.h>
#include <rs/fbs/Track_generated.h>
#include <rs/reactive/ItemList.h>

#include <filesystem>
#include <memory>
#include <sigc++/sigc++.h>

class PlaylistExporter
{
public:
  using TrackId = rs::core::MusicLibrary::TrackId;
  using AbstractTrackList = rs::reactive::ItemList<TrackId, rs::fbs::TrackT>;

  PlaylistExporter(AbstractTrackList& list, std::filesystem::path root, std::filesystem::path path);
  ~PlaylistExporter();

  // Methods to trigger write (to be called from outside)
  void triggerWrite();

private:
  void writeFile();
  void scheduleForWrite();

  AbstractTrackList& _list;
  std::filesystem::path const _root;
  std::filesystem::path const _path;
  std::unique_ptr<sigc::connection> _timeoutConnection;
};
