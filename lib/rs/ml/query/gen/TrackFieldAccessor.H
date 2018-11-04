// THIS IS A GENERATED FILE, DO NOT EDIT!!!

#pragma once

#include <rs/ml/core/Track.H>
#include <rs/ml/core/DataValue.H>
#include <rs/ml/core/CustomMeta.H>
#include <unordered_map>

class TrackFieldAccessor
{
public:
  static int name2Id(const std::string& name)
  {
    static std::unordered_map<std::string, int> name2IdMap = 
    {
      {"title", 0},
      {"t", 0},
      {"artist", 1},
      {"a", 1},
      {"album", 2},
      {"al", 2},
      {"albumArtist", 3},
      {"aa", 3},
      {"genre", 4},
      {"g", 4},
      {"trackNumber", 5},
      {"totalTracks", 6},
      {"discNumber", 7},
      {"totalDiscs", 8},
      {"filepath", 9},
      {"lastModified", 10},
      {"length", 11},
      {"l", 11},
      {"bitRate", 12},
      {"br", 12},
      {"sampleRate", 13},
      {"sr", 13},
      {"bitDepth", 14},
      {"bd", 14},
      {"tags", 15},
      {"custom", 16},
    };

    auto iter = name2IdMap.find(name);
    return iter != name2IdMap.end() ? iter->second : -1;
  };

  static rs::ml::core::DataValue get(const rs::ml::core::Track* track, int fieldId, const char* name)
  {
    switch (fieldId)
    {
      case -1:
        return rs::ml::core::customMeta(track, name);
      case 0:
      {
        auto val = track->title();
        return val == nullptr ? rs::ml::core::DataValue{} : std::string_view{val->c_str(), val->size()};
      }
      case 1:
      {
        auto val = track->artist();
        return val == nullptr ? rs::ml::core::DataValue{} : std::string_view{val->c_str(), val->size()};
      }
      case 2:
      {
        auto val = track->album();
        return val == nullptr ? rs::ml::core::DataValue{} : std::string_view{val->c_str(), val->size()};
      }
      case 3:
      {
        auto val = track->albumArtist();
        return val == nullptr ? rs::ml::core::DataValue{} : std::string_view{val->c_str(), val->size()};
      }
      case 4:
      {
        auto val = track->genre();
        return val == nullptr ? rs::ml::core::DataValue{} : std::string_view{val->c_str(), val->size()};
      }
      case 5:
        return {static_cast<std::int64_t>(track->trackNumber())};
      case 6:
        return {static_cast<std::int64_t>(track->totalTracks())};
      case 7:
        return {static_cast<std::int64_t>(track->discNumber())};
      case 8:
        return {static_cast<std::int64_t>(track->totalDiscs())};
      case 9:
      {
        auto val = track->filepath();
        return val == nullptr ? rs::ml::core::DataValue{} : std::string_view{val->c_str(), val->size()};
      }
      case 10:
        return {static_cast<std::int64_t>(track->lastModified())};
      case 11:
        return {static_cast<std::int64_t>(track->length())};
      case 12:
        return {static_cast<std::int64_t>(track->bitRate())};
      case 13:
        return {static_cast<std::int64_t>(track->sampleRate())};
      case 14:
        return {static_cast<std::int64_t>(track->bitDepth())};
      default: 
        return {};
    }
  }

  static rs::ml::core::DataValue get(const rs::ml::core::TrackT& track, int fieldId, const char* name)
  {
    switch (fieldId)
    {
      case -1:
        return rs::ml::core::customMeta(track, name);
      case 0:
        return track.title.empty() ? rs::ml::core::DataValue{} : std::string_view{track.title};
      case 1:
        return track.artist.empty() ? rs::ml::core::DataValue{} : std::string_view{track.artist};
      case 2:
        return track.album.empty() ? rs::ml::core::DataValue{} : std::string_view{track.album};
      case 3:
        return track.albumArtist.empty() ? rs::ml::core::DataValue{} : std::string_view{track.albumArtist};
      case 4:
        return track.genre.empty() ? rs::ml::core::DataValue{} : std::string_view{track.genre};
      case 5:
        return {static_cast<std::int64_t>(track.trackNumber)};
      case 6:
        return {static_cast<std::int64_t>(track.totalTracks)};
      case 7:
        return {static_cast<std::int64_t>(track.discNumber)};
      case 8:
        return {static_cast<std::int64_t>(track.totalDiscs)};
      case 9:
        return track.filepath.empty() ? rs::ml::core::DataValue{} : std::string_view{track.filepath};
      case 10:
        return {static_cast<std::int64_t>(track.lastModified)};
      case 11:
        return {static_cast<std::int64_t>(track.length)};
      case 12:
        return {static_cast<std::int64_t>(track.bitRate)};
      case 13:
        return {static_cast<std::int64_t>(track.sampleRate)};
      case 14:
        return {static_cast<std::int64_t>(track.bitDepth)};
      default: 
        return {};
    }
  }
};
