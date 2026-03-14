#include "PlaylistExporter.h"

#include <fstream>
#include <glibmm.h>
#include <iostream>

PlaylistExporter::PlaylistExporter(AbstractTrackList& list, std::filesystem::path root, std::filesystem::path path)
  : _list{list}
  , _root{std::move(root)}
  , _path{std::move(path)}
{
  scheduleForWrite();
}

PlaylistExporter::~PlaylistExporter()
{
  if (_timeoutConnection)
  {
    _timeoutConnection->disconnect();
  }
}

void PlaylistExporter::scheduleForWrite()
{
  // Cancel any existing timeout
  if (_timeoutConnection)
  {
    _timeoutConnection->disconnect();
  }

  // Schedule write after 1 second delay (Glib::signal_timeout uses milliseconds)
  _timeoutConnection = std::make_unique<sigc::connection>(Glib::signal_timeout().connect(
    [this]() {
      writeFile();
      return false;
    },
    1000));
}

void PlaylistExporter::writeFile()
{
  _timeoutConnection->disconnect();
  _timeoutConnection.reset();

  auto ofs = std::ofstream{_path};

  if (!ofs)
  {
    std::cerr << "Failed to open playlist file: " << _path << std::endl;
    return;
  }

  for (auto i = 0u; i < _list.size(); ++i)
  {
    const auto& [_, track] = _list.at(AbstractTrackList::Index{i});
    auto relativePath = std::filesystem::relative(_root / track.prop->filepath, _path.parent_path());
    ofs << relativePath.string() << std::endl;
  }
}

void PlaylistExporter::triggerWrite() { scheduleForWrite(); }
