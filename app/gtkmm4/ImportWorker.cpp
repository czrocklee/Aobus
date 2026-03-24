// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "ImportWorker.h"

#include <rs/tag/flac/File.h>
#include <rs/tag/mp4/File.h>
#include <rs/tag/mpeg/File.h>

#include <flatbuffers/flatbuffers.h>

#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
  std::unique_ptr<rs::tag::File> createTagFileByExtension(std::filesystem::path const& path)
  {
    using namespace rs::tag;
    static std::unordered_map<std::string, std::function<std::unique_ptr<File>(std::filesystem::path const)>> const
      CreatorMap = {
        {".mp3", [](auto const& path) { return std::make_unique<mpeg::File>(path, File::Mode::ReadOnly); }},
        {".m4a", [](auto const& path) { return std::make_unique<mp4::File>(path, File::Mode::ReadOnly); }},
        {".flac", [](auto const& path) { return std::make_unique<flac::File>(path, File::Mode::ReadOnly); }}};

    return std::invoke(CreatorMap.at(path.extension().string()), path);
  }

  ::flatbuffers::Offset<::flatbuffers::String> buildString(::flatbuffers::FlatBufferBuilder& fbb,
                                                           rs::tag::ValueType const& value)
  {
    return rs::tag::isNull(value) ? ::flatbuffers::Offset<::flatbuffers::String>{}
                                  : fbb.CreateString(std::get<std::string>(value));
  }
}

ImportWorker::ImportWorker(rs::core::MusicLibrary& ml,
                           std::vector<std::filesystem::path> const& files,
                           ProgressCallback progressCallback,
                           FinishedCallback finishedCallback)
  : _ml{ml}
  , _txn{ml.writeTransaction()}
  , _files{files}
  , _rootPathStr{ml.rootPath().string()}
  , _progressCallback{progressCallback}
  , _finishedCallback{finishedCallback}
{
}

void ImportWorker::run()
{
  auto trackWriter = _ml.tracks().writer(_txn.value());
  auto resourceWriter = _ml.resources().writer(_txn.value());

  for (auto i = 0u; i < _files.size(); ++i)
  {
    try
    {
      auto const& path = _files[i];

      // Report progress
      if (_progressCallback)
      {
        _progressCallback(path, i);
      }

      auto const file = createTagFileByExtension(path);
      rs::tag::Metadata metadata;
      metadata = file->loadMetadata();

      auto [id, track] = trackWriter.create([&](::flatbuffers::FlatBufferBuilder& fbb) {
        ::flatbuffers::Offset<rs::fbs::Metadata> metaOffset;

        {
          auto titleOffset = buildString(fbb, metadata.get(rs::tag::MetaField::Title));
          auto albumOffset = buildString(fbb, metadata.get(rs::tag::MetaField::Album));
          auto artistOffset = buildString(fbb, metadata.get(rs::tag::MetaField::Artist));
          auto albumArtistOffset = buildString(fbb, metadata.get(rs::tag::MetaField::AlbumArtist));
          auto genreOffset = buildString(fbb, metadata.get(rs::tag::MetaField::Genre));

          auto builder = rs::fbs::MetadataBuilder{fbb};
          builder.add_title(titleOffset);
          builder.add_album(albumOffset);
          builder.add_artist(artistOffset);
          builder.add_albumArtist(albumArtistOffset);
          builder.add_genre(genreOffset);
          metaOffset = builder.Finish();
        }

        ::flatbuffers::Offset<rs::fbs::Properties> propOffset;

        {
          auto filepathOffset =
            fbb.CreateString(std::filesystem::relative(path, std::filesystem::path(_rootPathStr)).string());
          // Convert file_time_type to system_clock - use portable approach
          auto ftime = std::filesystem::last_write_time(path);
          auto epochInNanos = std::chrono::duration_cast<std::chrono::nanoseconds>(ftime.time_since_epoch()).count();

          auto builder = rs::fbs::PropertiesBuilder{fbb};
          builder.add_filepath(filepathOffset);
          builder.add_lastModified(epochInNanos);
          propOffset = builder.Finish();
        }

        std::vector<::flatbuffers::Offset<rs::fbs::Resource>> rsrc;

        if (auto albumArt = metadata.get(rs::tag::MetaField::AlbumArt); !rs::tag::isNull(albumArt))
        {
          auto const& blob = std::get<rs::tag::Blob>(albumArt);
          std::uint64_t id = resourceWriter.create(boost::asio::buffer(blob.data(), blob.size()));
          std::cout << "id " << id << std::endl;
          auto builder = rs::fbs::ResourceBuilder{fbb};
          builder.add_type(rs::fbs::ResourceType::AlbumArt);
          builder.add_id(id);
          rsrc.push_back(builder.Finish());
        }

        auto rsrcOffset = fbb.CreateVector(rsrc);

        std::vector<std::string> t{"tag", "tag1", "tag2"};
        [[maybe_unused]] auto tags = fbb.CreateVectorOfStrings(t);
        std::vector<flatbuffers::Offset<rs::fbs::CustomEntry>> entries;
        entries.push_back(rs::fbs::CreateCustomEntry(fbb, fbb.CreateString("pop")));
        entries.push_back(rs::fbs::CreateCustomEntry(fbb, fbb.CreateString("classic")));
        auto custom = fbb.CreateVectorOfSortedTables(&entries);

        auto builder = rs::fbs::TrackBuilder{fbb};
        builder.add_meta(metaOffset);
        builder.add_prop(propOffset);
        builder.add_rsrc(rsrcOffset);
        builder.add_custom(custom);
        return builder.Finish();
      });
    }
    catch ([[maybe_unused]] std::exception const& e)
    {
      // std::cerr << "failed to parse metadata " << e.what() << std::endl;
      continue;
    }
  }

  if (_finishedCallback)
  {
    _finishedCallback();
  }
}

void ImportWorker::commit()
{
  _txn.value().commit();
  _txn.reset();
}
