// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "runtime/library/LibraryYamlExporter.h"

#include "MediaTrack.h"
#include <ao/AudioCodec.h>
#include <ao/AudioCodecText.h>
#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/OperationCancelled.h>
#include <ao/library/CoverArt.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/LibraryUri.h>
#include <ao/library/ListStore.h>
#include <ao/library/ListView.h>
#include <ao/library/MetadataLayout.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/ResourceStore.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/library/LibraryTransfer.h>
#include <ao/utility/AtomicFile.h>
#include <ao/utility/Path.h>
#include <ao/utility/Sha256.h>
#include <ao/utility/Uuid.h>
#include <ao/yaml/RymlAdapter.h>

#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace ao::rt
{
  namespace
  {
    using MetadataStringGetter = std::string_view (*)(library::TrackView const&, library::DictionaryStore const&);
    using MetadataStringBaseGetter = std::string_view (*)(library::TrackBuilder::MetadataBuilder const&);
    using MetadataNumberGetter = std::uint16_t (*)(library::TrackView const&);
    using MetadataNumberBaseGetter = std::uint16_t (*)(library::TrackBuilder::MetadataBuilder const&);

    struct MetadataDispatch final
    {
      TrackField field;
      MetadataStringGetter stringGetter = nullptr;
      MetadataStringBaseGetter baseStringGetter = nullptr;
      MetadataNumberGetter numberGetter = nullptr;
      MetadataNumberBaseGetter baseNumberGetter = nullptr;
    };

    constexpr auto kMetadataDispatch = std::to_array<MetadataDispatch>({
      {.field = TrackField::Title,
       .stringGetter = [](auto const& view, auto&) { return view.metadata().title(); },
       .baseStringGetter = [](auto const& base) { return base.title(); }},
      {.field = TrackField::Artist,
       .stringGetter =
         [](auto const& view, auto& dictionary)
       {
         auto const id = view.metadata().artistId();
         return id != kInvalidDictionaryId ? dictionary.get(id) : std::string_view{};
       },
       .baseStringGetter = [](auto const& base) { return base.artist(); }},
      {.field = TrackField::Album,
       .stringGetter =
         [](auto const& view, auto& dictionary)
       {
         auto const id = view.metadata().albumId();
         return id != kInvalidDictionaryId ? dictionary.get(id) : std::string_view{};
       },
       .baseStringGetter = [](auto const& base) { return base.album(); }},
      {.field = TrackField::AlbumArtist,
       .stringGetter =
         [](auto const& view, auto& dictionary)
       {
         auto const id = view.metadata().albumArtistId();
         return id != kInvalidDictionaryId ? dictionary.get(id) : std::string_view{};
       },
       .baseStringGetter = [](auto const& base) { return base.albumArtist(); }},
      {.field = TrackField::Composer,
       .stringGetter =
         [](auto const& view, auto& dictionary)
       {
         auto const id = view.metadata().composerId();
         return id != kInvalidDictionaryId ? dictionary.get(id) : std::string_view{};
       },
       .baseStringGetter = [](auto const& base) { return base.composer(); }},
      {.field = TrackField::Conductor,
       .stringGetter =
         [](auto const& view, auto& dictionary)
       {
         auto const id = view.classical().conductorId();
         return id != kInvalidDictionaryId ? dictionary.get(id) : std::string_view{};
       },
       .baseStringGetter = [](auto const& base) { return base.conductor(); }},
      {.field = TrackField::Ensemble,
       .stringGetter =
         [](auto const& view, auto& dictionary)
       {
         auto const id = view.classical().ensembleId();
         return id != kInvalidDictionaryId ? dictionary.get(id) : std::string_view{};
       },
       .baseStringGetter = [](auto const& base) { return base.ensemble(); }},
      {.field = TrackField::Genre,
       .stringGetter =
         [](auto const& view, auto& dictionary)
       {
         auto const id = view.metadata().genreId();
         return id != kInvalidDictionaryId ? dictionary.get(id) : std::string_view{};
       },
       .baseStringGetter = [](auto const& base) { return base.genre(); }},
      {.field = TrackField::Work,
       .stringGetter =
         [](auto const& view, auto& dictionary)
       {
         auto const id = view.classical().workId();
         return id != kInvalidDictionaryId ? dictionary.get(id) : std::string_view{};
       },
       .baseStringGetter = [](auto const& base) { return base.work(); }},
      {.field = TrackField::Movement,
       .stringGetter =
         [](auto const& view, auto& dictionary)
       {
         auto const id = view.classical().movementId();
         return id != kInvalidDictionaryId ? dictionary.get(id) : std::string_view{};
       },
       .baseStringGetter = [](auto const& base) { return base.movement(); }},
      {.field = TrackField::Soloist,
       .stringGetter =
         [](auto const& view, auto& dictionary)
       {
         auto const id = view.classical().soloistId();
         return id != kInvalidDictionaryId ? dictionary.get(id) : std::string_view{};
       },
       .baseStringGetter = [](auto const& base) { return base.soloist(); }},
      {.field = TrackField::Year,
       .numberGetter = [](auto const& view) { return view.metadata().year(); },
       .baseNumberGetter = [](auto const& base) { return base.year(); }},
      {.field = TrackField::TrackNumber,
       .numberGetter = [](auto const& view) { return view.metadata().trackNumber(); },
       .baseNumberGetter = [](auto const& base) { return base.trackNumber(); }},
      {.field = TrackField::TrackTotal,
       .numberGetter = [](auto const& view) { return view.metadata().trackTotal(); },
       .baseNumberGetter = [](auto const& base) { return base.trackTotal(); }},
      {.field = TrackField::DiscNumber,
       .numberGetter = [](auto const& view) { return view.metadata().discNumber(); },
       .baseNumberGetter = [](auto const& base) { return base.discNumber(); }},
      {.field = TrackField::DiscTotal,
       .numberGetter = [](auto const& view) { return view.metadata().discTotal(); },
       .baseNumberGetter = [](auto const& base) { return base.discTotal(); }},
      {.field = TrackField::MovementNumber,
       .numberGetter = [](auto const& view) { return view.classical().movementNumber(); },
       .baseNumberGetter = [](auto const& base) { return base.movementNumber(); }},
      {.field = TrackField::MovementTotal,
       .numberGetter = [](auto const& view) { return view.classical().movementTotal(); },
       .baseNumberGetter = [](auto const& base) { return base.movementTotal(); }},
    });

    void appendString(ryml::NodeRef& node, std::string_view key, std::string_view value)
    {
      auto child = node.append_child();
      yaml::setKey(child, key);
      yaml::setValue(child, value);
    }

    void emitTrackMetadata(ryml::NodeRef& node,
                           library::TrackView const& view,
                           library::DictionaryStore const& dictionary,
                           std::optional<library::TrackBuilder> const& optBaseline)
    {
      auto const optBaselineMetadata = optBaseline ? std::optional{optBaseline->metadata()} : std::nullopt;
      auto const hasBaseline = optBaselineMetadata.has_value();

      for (auto const& map : kMetadataDispatch)
      {
        if (auto const key = trackFieldId(map.field); map.stringGetter != nullptr)
        {
          auto const current = map.stringGetter(view, dictionary);
          bool const shouldEmit =
            hasBaseline ? (current != map.baseStringGetter(*optBaselineMetadata)) : !current.empty();

          if (shouldEmit)
          {
            appendString(node, key, current);
          }
        }
        else if (map.numberGetter != nullptr)
        {
          auto const current = map.numberGetter(view);
          bool const shouldEmit =
            hasBaseline ? (current != map.baseNumberGetter(*optBaselineMetadata)) : (current != 0);

          if (shouldEmit)
          {
            node.append_child() << ryml::key(key) << current;
          }
        }
      }

      if (auto const custom = view.customMetadata(); !custom.empty())
      {
        auto customNode = node.append_child();
        yaml::setKey(customNode, "custom");
        customNode |= ryml::MAP;

        for (auto const& [dictionaryId, value] : custom)
        {
          auto child = customNode.append_child();
          yaml::setKey(child, dictionary.get(dictionaryId));
          yaml::setValue(child, value);
        }
      }
    }

    using PropertyU64Getter = std::uint64_t (*)(library::TrackView::PropertyProxy const&);
    using PropertyU32Getter = std::uint32_t (*)(library::TrackView::PropertyProxy const&);
    using PropertyU16Getter = std::uint16_t (*)(library::TrackView::PropertyProxy const&);
    using PropertyU8Getter = std::uint8_t (*)(library::TrackView::PropertyProxy const&);
    using PropertyStringGetter = std::string_view (*)(library::TrackView::PropertyProxy const&);

    struct PropertyDispatch final
    {
      TrackField field;
      PropertyU64Getter u64Get = nullptr;
      PropertyU32Getter u32Get = nullptr;
      PropertyU16Getter u16Get = nullptr;
      PropertyU8Getter u8Get = nullptr;
      PropertyStringGetter stringGet = nullptr;
    };

    constexpr auto kPropertyDispatch = std::to_array<PropertyDispatch>({
      {.field = TrackField::Duration,
       .u32Get = [](auto const& prop) { return static_cast<std::uint32_t>(prop.duration().count()); }},
      {.field = TrackField::Bitrate, .u32Get = [](auto const& prop) { return prop.bitrate().raw(); }},
      {.field = TrackField::SampleRate, .u32Get = [](auto const& prop) { return prop.sampleRate().raw(); }},
      {.field = TrackField::Codec,
       .stringGet = [](auto const& prop)
       { return prop.codec() == AudioCodec::Unknown ? std::string_view{"UNKNOWN"} : audioCodecName(prop.codec()); }},
      {.field = TrackField::Channels, .u8Get = [](auto const& prop) { return prop.channels().raw(); }},
      {.field = TrackField::BitDepth, .u8Get = [](auto const& prop) { return prop.bitDepth().raw(); }},
    });

    Result<> emitTrackProperties(ryml::NodeRef& node,
                                 library::TrackView::PropertyProxy const& property,
                                 library::FileManifestStore::Reader const& manifestReader)
    {
      for (auto const& map : kPropertyDispatch)
      {
        if (auto const key = trackFieldId(map.field); map.u64Get != nullptr)
        {
          node.append_child() << ryml::key(key) << map.u64Get(property);
        }
        else if (map.u32Get != nullptr)
        {
          node.append_child() << ryml::key(key) << map.u32Get(property);
        }
        else if (map.u16Get != nullptr)
        {
          node.append_child() << ryml::key(key) << map.u16Get(property);
        }
        else if (map.u8Get != nullptr)
        {
          node.append_child() << ryml::key(key) << map.u8Get(property);
        }
        else if (map.stringGet != nullptr)
        {
          node.append_child() << ryml::key(key) << map.stringGet(property);
        }
      }

      std::uint64_t fileSize = 0;
      std::uint64_t mtime = 0;

      if (auto const optManifest = manifestReader.get(property.uri()); optManifest)
      {
        fileSize = optManifest->fileSize();
        mtime = optManifest->mtime();
      }

      node.append_child() << ryml::key("fileSize") << fileSize;
      node.append_child() << ryml::key("mtime") << mtime;
      return {};
    }

    /**
     * Which covers a document records, and what it names them by.
     *
     * A cover reference belongs to the library rather than to the file, so a
     * `full` document records the whole graph: which digest each track names, in
     * what order, with what picture type. `metadata` and `delta` record none.
     *
     * `metadata` omits them because it holds what a scan cannot rebuild, and an
     * embedded cover is not that: both its bytes and its identity come from the
     * file. `delta` omits them because the library holds what the last scan saw,
     * so a file retagged since then makes the two differ and the sequence delta
     * would carry is the stale one — applying it would overwrite the covers the
     * baseline just read from the file with references to content that may exist
     * nowhere.
     */
    bool recordsCovers(ExportMode const mode)
    {
      return mode == ExportMode::Full;
    }

    /// One row of `library.resources`, keyed so iteration is ascending by digest.
    using ReachableDescriptorMap = std::map<utility::Sha256Digest, std::uint32_t>;

    /// The digest text each referenced handle resolves to, so a cover reference
    /// costs a lookup rather than a read and a format per cover.
    using CoverDigestTextMap = std::unordered_map<ResourceId, std::string>;

    struct ReachableResources final
    {
      ReachableDescriptorMap descriptors{};
      CoverDigestTextMap digestText{};
    };

    /**
     * @brief The descriptors the exported tracks reach, and nothing else.
     *
     * Descriptors are append-only, so a scanned library accumulates rows no track
     * references any more. A document is a record of a library's live state, not
     * of everything it has ever seen, and a restore that recreated the dead rows
     * would be reconstructing history rather than content.
     */
    ReachableResources collectReachableResources(library::TrackStore::Reader const& trackReader,
                                                 library::ResourceStore::Reader const& resourceReader,
                                                 std::stop_token const& stopToken)
    {
      auto reachable = ReachableResources{};

      for (auto const& [trackId, view] : trackReader.cold())
      {
        async::throwIfStopRequested(stopToken);

        for (auto const cover : view.coverArt())
        {
          if (reachable.digestText.contains(cover.resourceId))
          {
            continue;
          }

          auto const optDescriptor = resourceReader.get(cover.resourceId);
          AO_INVARIANT(optDescriptor,
                       "Track {} cover references missing Resource {} after library validation",
                       trackId.raw(),
                       cover.resourceId.raw());
          reachable.descriptors.insert_or_assign(optDescriptor->digest, optDescriptor->byteLength);
          reachable.digestText.emplace(cover.resourceId, utility::sha256Hex(optDescriptor->digest));
        }
      }

      return reachable;
    }

    /// Emits `library.resources`, in ascending digest order so two exports of one
    /// unchanged library are byte-identical.
    void emitResourceTable(ryml::NodeRef& node, ReachableDescriptorMap const& descriptors)
    {
      auto resourcesNode = node.append_child();
      yaml::setKey(resourcesNode, "resources");
      resourcesNode |= ryml::SEQ;

      for (auto const& [digest, byteLength] : descriptors)
      {
        auto rowNode = resourcesNode.append_child();
        rowNode |= ryml::MAP;
        appendString(rowNode, "digest", utility::sha256Hex(digest));
        rowNode.append_child() << ryml::key("length") << byteLength;
      }
    }

    void emitTrackCover(ryml::NodeRef& node,
                        library::TrackView const& view,
                        ExportMode const mode,
                        CoverDigestTextMap const& digestText)
    {
      if (!recordsCovers(mode))
      {
        return;
      }

      auto coversNode = node.append_child();
      yaml::setKey(coversNode, "covers");
      coversNode |= ryml::SEQ;

      auto const covers = view.coverArt();

      for (std::uint16_t i = 0; i < covers.count(); ++i)
      {
        auto const cover = covers.at(i);
        auto coverNode = coversNode.append_child();
        coverNode |= ryml::MAP;
        coverNode.append_child() << ryml::key("type") << static_cast<std::uint32_t>(cover.type);

        // The reference is the digest itself and never a ResourceId: a handle is
        // local to the library that minted it and means nothing in a document.
        auto const found = digestText.find(cover.resourceId);
        AO_INVARIANT(found != digestText.end(),
                     "Cover Resource {} is missing from the collected reachable set",
                     cover.resourceId.raw());
        appendString(coverNode, "resource", found->second);
      }
    }

    void emitTrackCommon(ryml::NodeRef& node,
                         library::TrackView::TagProxy const& tags,
                         library::DictionaryStore const& dictionary)
    {
      if (tags.count() != 0)
      {
        auto tagsNode = node.append_child();
        yaml::setKey(tagsNode, "tags");
        tagsNode |= ryml::SEQ;

        for (auto const tagId : tags)
        {
          yaml::setValue(tagsNode.append_child(), dictionary.get(tagId));
        }
      }
    }

    Result<> emitList(ryml::NodeRef& listsNode,
                      ListId const listId,
                      library::ListView const& listView,
                      ExportMode const mode,
                      library::TrackStore::Reader const& trackReader)
    {
      auto listNode = listsNode.append_child();
      listNode |= ryml::MAP;

      listNode.append_child() << ryml::key("id") << listId.raw();
      listNode.append_child() << ryml::key("parentId") << listView.parentId().raw();
      appendString(listNode, "name", listView.name());

      if (!listView.description().empty())
      {
        appendString(listNode, "description", listView.description());
      }

      if (!listView.filter().empty())
      {
        appendString(listNode, "filter", listView.filter());
      }

      auto const orderTrackIds = listView.orderTrackIds();

      if (orderTrackIds.empty())
      {
        return {};
      }

      auto orderNode = listNode.append_child();
      yaml::setKey(orderNode, "order");
      orderNode |= ryml::SEQ;

      for (auto const trackId : orderTrackIds)
      {
        if (mode != ExportMode::ListOnly)
        {
          orderNode.append_child() << trackId.raw();
          continue;
        }

        auto const optTrackView = trackReader.get(trackId, library::TrackStore::Reader::LoadMode::Cold);

        if (!optTrackView)
        {
          continue;
        }

        auto uriRes = library::LibraryUri::parse(optTrackView->property().uri());
        AO_INVARIANT(uriRes && uriRes->value() == optTrackView->property().uri(),
                     "Track {} contains a non-canonical URI after library validation",
                     trackId.raw());

        auto refNode = orderNode.append_child();
        refNode |= ryml::MAP;
        appendString(refNode, "uri", uriRes->value());
      }

      return {};
    }
  } // namespace

  struct LibraryYamlExporter::Impl final
  {
    explicit Impl(library::MusicLibrary const& ml)
      : ml{ml}
    {
    }

    Result<> exportToYaml(std::filesystem::path const& path, ExportMode mode, std::stop_token const& stopToken) const;
    Result<> exportTracks(ryml::NodeRef& node,
                          library::ReadTransaction const& transaction,
                          ExportMode mode,
                          std::stop_token const& stopToken) const;
    Result<> exportTrack(ryml::NodeRef& node,
                         TrackId id,
                         library::TrackView const& view,
                         ExportMode mode,
                         CoverDigestTextMap const& digestText,
                         library::DictionaryStore const& dictionary,
                         library::FileManifestStore::Reader const& manifestReader) const;
    Result<> exportLists(ryml::NodeRef& node,
                         library::ReadTransaction const& transaction,
                         ExportMode mode,
                         std::stop_token const& stopToken) const;

    library::MusicLibrary const& ml;
  };

  LibraryYamlExporter::LibraryYamlExporter(library::MusicLibrary const& library)
    : _implPtr{std::make_unique<Impl>(library)}
  {
  }

  LibraryYamlExporter::~LibraryYamlExporter() = default;

  LibraryYamlExporter::LibraryYamlExporter(LibraryYamlExporter&&) noexcept = default;

  LibraryYamlExporter& LibraryYamlExporter::operator=(LibraryYamlExporter&&) noexcept = default;

  Result<> LibraryYamlExporter::exportToYaml(std::filesystem::path const& path,
                                             ExportMode mode,
                                             std::stop_token stopToken)
  {
    return _implPtr->exportToYaml(path, mode, stopToken);
  }

  Result<> LibraryYamlExporter::Impl::exportToYaml(std::filesystem::path const& path,
                                                   ExportMode mode,
                                                   std::stop_token const& stopToken) const
  {
    auto tree = ryml::Tree{};
    auto root = tree.rootref();
    root |= ryml::MAP;

    auto const transaction = ml.readTransaction();
    auto const header = ml.metadataHeader(transaction);

    root.append_child() << ryml::key("version") << kYamlFormatVersion;
    appendString(root, "libraryId", utility::formatUuid(header.libraryId));
    appendString(root, "export_mode", exportModeName(mode));

    auto library = root.append_child();
    yaml::setKey(library, "library");
    library |= ryml::MAP;

    if (mode != ExportMode::ListOnly)
    {
      if (auto result = exportTracks(library, transaction, mode, stopToken); !result)
      {
        return result;
      }
    }

    if (auto result = exportLists(library, transaction, mode, stopToken); !result)
    {
      return result;
    }

    // The destination directory is the user's to name, not this exporter's to
    // create: a path below a directory that does not exist is a mistyped
    // destination far more often than a request to build a tree, and the atomic
    // write below would build it.
    auto directoryError = std::error_code{};

    if (auto const parent = path.parent_path();
        !parent.empty() && !std::filesystem::is_directory(parent, directoryError))
    {
      return makeError(
        Error::Code::IoError, std::format("Export directory does not exist: '{}'", utility::pathToUtf8(parent)));
    }

    std::string const yaml = ryml::emitrs_yaml<std::string>(tree);

    // Written whole or not at all. A user exports over the backup they already
    // have, so a truncating stream would spend the old document before knowing
    // it could produce the new one, and a cancellation, a full disk, or a crash
    // would leave neither.
    return utility::writeAtomically(path, yaml);
  }

  Result<> LibraryYamlExporter::Impl::exportTracks(ryml::NodeRef& node,
                                                   library::ReadTransaction const& transaction,
                                                   ExportMode mode,
                                                   std::stop_token const& stopToken) const
  {
    auto const trackReader = ml.tracks().reader(transaction);
    auto const manifestReader = ml.manifest().reader(transaction);
    auto const& dictionary = ml.dictionary();

    // The table is collected before the tracks that name it so it can be emitted
    // first, and because a cover reference is emitted as its digest rather than
    // as the handle the track record stores.
    auto reachable = ReachableResources{};

    if (recordsCovers(mode))
    {
      reachable = collectReachableResources(trackReader, ml.resources().reader(transaction), stopToken);
      emitResourceTable(node, reachable.descriptors);
    }

    auto tracksNode = node.append_child();
    yaml::setKey(tracksNode, "tracks");
    tracksNode |= ryml::SEQ;

    for (auto const& [trackId, view] : trackReader)
    {
      // A record at a time is the natural granularity: the walk holds a read
      // transaction and an emitted tree, and both are dropped whole.
      async::throwIfStopRequested(stopToken);

      if (auto result = exportTrack(tracksNode, trackId, view, mode, reachable.digestText, dictionary, manifestReader);
          !result)
      {
        return result;
      }
    }

    return {};
  }

  Result<> LibraryYamlExporter::Impl::exportTrack(ryml::NodeRef& node,
                                                  TrackId id,
                                                  library::TrackView const& view,
                                                  ExportMode mode,
                                                  CoverDigestTextMap const& digestText,
                                                  library::DictionaryStore const& dictionary,
                                                  library::FileManifestStore::Reader const& manifestReader) const
  {
    auto trackNode = node.append_child();
    trackNode |= ryml::MAP;

    trackNode.append_child() << ryml::key("id") << id.raw();

    auto const property = view.property();
    auto uriRes = library::LibraryUri::parse(property.uri());
    AO_INVARIANT(uriRes && uriRes->value() == property.uri(),
                 "Track {} contains a non-canonical URI after library validation",
                 id.raw());

    appendString(trackNode, "uri", uriRes->value());

    auto optMediaTrack = std::optional<MediaTrack>{};
    auto optBaseline = std::optional<library::TrackBuilder>{};

    if (mode == ExportMode::Delta)
    {
      auto fileEc = std::error_code{};

      auto fullPathRes = uriRes->resolveUnder(ml.rootPath());

      if (!fullPathRes)
      {
        return std::unexpected{fullPathRes.error()};
      }

      if (auto const& fullPath = *fullPathRes; std::filesystem::exists(fullPath, fileEc) && !fileEc)
      {
        if (auto mediaTrackRes = readMediaTrack(fullPath); mediaTrackRes)
        {
          optMediaTrack.emplace(std::move(*mediaTrackRes));
          optBaseline = optMediaTrack->builder();
        }
      }
      else if (fileEc)
      {
        return makeError(
          Error::Code::IoError,
          std::format("Failed to inspect file '{}': {}", utility::pathToUtf8(fullPath), fileEc.message()));
      }
    }

    if (mode != ExportMode::ListOnly)
    {
      emitTrackMetadata(trackNode, view, dictionary, optBaseline);
    }

    if (mode == ExportMode::Full)
    {
      if (auto result = emitTrackProperties(trackNode, property, manifestReader); !result)
      {
        return result;
      }
    }

    emitTrackCover(trackNode, view, mode, digestText);
    emitTrackCommon(trackNode, view.tags(), dictionary);
    return {};
  }

  Result<> LibraryYamlExporter::Impl::exportLists(ryml::NodeRef& node,
                                                  library::ReadTransaction const& transaction,
                                                  ExportMode mode,
                                                  std::stop_token const& stopToken) const
  {
    auto listsNode = node.append_child();
    yaml::setKey(listsNode, "lists");
    listsNode |= ryml::SEQ;

    auto const listReader = ml.lists().reader(transaction);
    auto const trackReader = ml.tracks().reader(transaction);

    for (auto const& [listId, listView] : listReader)
    {
      async::throwIfStopRequested(stopToken);

      if (auto result = emitList(listsNode, listId, listView, mode, trackReader); !result)
      {
        return result;
      }
    }

    return {};
  }
} // namespace ao::rt
