// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/AudioCodec.h>
#include <ao/AudioScalars.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/CoverArt.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListStore.h>
#include <ao/library/MetadataLayout.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackWrite.h>
#include <ao/lmdb/Transaction.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/library/LibraryYamlExporter.h>
#include <ao/rt/library/LibraryYamlImporter.h>
#include <ao/tag/TagFile.h>
#include <ao/utility/Base64.h>
#include <ao/yaml/RymlAdapter.h>

#include <c4/yml/node.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace ao::rt
{
  namespace
  {
    enum class ImportRunMode : std::uint8_t
    {
      Commit,
      Preview,
    };

    struct ValidatedTrack final
    {
      std::uint32_t yamlId = 0;
      std::string uri;
      ryml::ConstNodeRef node;
    };

    struct ValidatedList final
    {
      std::uint32_t yamlId = 0;
      std::uint32_t yamlParentId = 0;
      std::string_view name;
      std::string_view description;
      std::string_view filter;
      std::vector<std::uint32_t> yamlTrackIds;
      std::vector<std::string> trackUris;
      bool isSmart = false;
    };

    struct ValidatedImport final
    {
      std::uint32_t version = 0;
      ExportMode payloadMode = ExportMode::Full;
      std::optional<std::string_view> optLibraryId;
      std::vector<ValidatedTrack> tracks;
      std::vector<ValidatedList> lists;
    };

    Result<ryml::ConstNodeRef> requireField(ryml::ConstNodeRef const& node,
                                            std::string_view field,
                                            std::string_view context)
    {
      auto const child = yaml::findChild(node, field);

      if (!child.readable())
      {
        return makeError(Error::Code::FormatRejected, std::format("{} missing required '{}' field", context, field));
      }

      return child;
    }

    Result<std::string_view> requireScalar(ryml::ConstNodeRef const& node, std::string_view context)
    {
      if (!node.has_val())
      {
        return makeError(Error::Code::FormatRejected, std::format("{} must be a scalar", context));
      }

      return yaml::scalarView(node);
    }

    Result<std::string_view> requireScalarInFieldContext(ryml::ConstNodeRef const& node,
                                                         std::string_view context,
                                                         std::string_view field)
    {
      if (!node.has_val())
      {
        return makeError(Error::Code::FormatRejected, std::format("{}.{} must be a scalar", context, field));
      }

      return yaml::scalarView(node);
    }

    Result<std::string_view> requireScalarField(ryml::ConstNodeRef const& node,
                                                std::string_view field,
                                                std::string_view context)
    {
      auto child = requireField(node, field, context);

      if (!child)
      {
        return std::unexpected{child.error()};
      }

      return requireScalarInFieldContext(*child, context, field);
    }

    template<typename T>
    Result<T> requireScalarAs(ryml::ConstNodeRef const& node, std::string_view context)
    {
      auto value = yaml::scalarAs<T>(node, context);

      if (!value)
      {
        return std::unexpected{value.error()};
      }

      return *value;
    }

    template<typename T>
    Result<T> requireScalarInFieldContextAs(ryml::ConstNodeRef const& node,
                                            std::string_view context,
                                            std::string_view field)
    {
      if (T value = {}; yaml::tryReadScalar(node, value))
      {
        return value;
      }

      return makeError(Error::Code::FormatRejected, std::format("{}.{} must be a valid scalar", context, field));
    }

    template<typename T>
    Result<T> requireScalarFieldAs(ryml::ConstNodeRef const& node, std::string_view field, std::string_view context)
    {
      auto child = requireField(node, field, context);

      if (!child)
      {
        return std::unexpected{child.error()};
      }

      return requireScalarInFieldContextAs<T>(*child, context, field);
    }

    Result<> requireMap(ryml::ConstNodeRef const& node, std::string_view context)
    {
      if (!node.is_map())
      {
        return makeError(Error::Code::FormatRejected, std::format("{} must be a map", context));
      }

      return {};
    }

    Result<> requireSequence(ryml::ConstNodeRef const& node, std::string_view context)
    {
      if (!node.is_seq())
      {
        return makeError(Error::Code::FormatRejected, std::format("{} must be a sequence", context));
      }

      return {};
    }

    std::string normalizedUri(std::string_view uri)
    {
      auto rawUriStr = std::string{uri};
      std::ranges::replace(rawUriStr, '\\', '/');
      return std::filesystem::path{rawUriStr}.lexically_normal().generic_string();
    }

    bool isHexDigit(char ch) noexcept
    {
      return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
    }

    bool isUuidText(std::string_view uuidStr) noexcept
    {
      constexpr std::size_t kUuidLength = 36;
      constexpr std::size_t kFirstHyphen = 8;
      constexpr std::size_t kSecondHyphen = 13;
      constexpr std::size_t kThirdHyphen = 18;
      constexpr std::size_t kFourthHyphen = 23;

      if (uuidStr.size() != kUuidLength)
      {
        return false;
      }

      for (std::size_t index = 0; index < uuidStr.size(); ++index)
      {
        auto const expectsHyphen =
          index == kFirstHyphen || index == kSecondHyphen || index == kThirdHyphen || index == kFourthHyphen;

        if (expectsHyphen)
        {
          if (uuidStr[index] != '-')
          {
            return false;
          }
        }
        else if (!isHexDigit(uuidStr[index]))
        {
          return false;
        }
      }

      return true;
    }

    std::optional<ExportMode> parseExportMode(std::string_view modeStr)
    {
      if (modeStr == "delta" || modeStr == "minimum")
      {
        return ExportMode::Delta;
      }

      if (modeStr == "metadata")
      {
        return ExportMode::Metadata;
      }

      if (modeStr == "full")
      {
        return ExportMode::Full;
      }

      if (modeStr == "listOnly")
      {
        return ExportMode::ListOnly;
      }

      return std::nullopt;
    }

    constexpr std::uint8_t hexDigit(char ch)
    {
      if (ch >= '0' && ch <= '9')
      {
        return static_cast<std::uint8_t>(ch - '0');
      }

      constexpr std::uint8_t kHexBase = 10;

      if (ch >= 'a' && ch <= 'f')
      {
        return static_cast<std::uint8_t>(ch - 'a' + kHexBase);
      }

      if (ch >= 'A' && ch <= 'F')
      {
        return static_cast<std::uint8_t>(ch - 'A' + kHexBase);
      }

      return 0;
    }

    std::array<std::byte, 16> parseUuid(std::string_view uuidStr)
    {
      auto result = std::array<std::byte, 16>{};
      std::int32_t byteIndex = 0;

      for (std::size_t i = 0; i < uuidStr.size() && byteIndex < 16; ++i)
      {
        if (uuidStr[i] == '-')
        {
          continue;
        }

        if (i + 1 < uuidStr.size())
        {
          auto const hi = hexDigit(uuidStr[i]);
          auto const lo = hexDigit(uuidStr[i + 1]);
          result.at(static_cast<std::size_t>(byteIndex++)) = static_cast<std::byte>((hi << 4U) | lo);
          ++i;
        }
      }

      return result;
    }
  } // namespace

  struct LibraryYamlImporter::Impl final
  {
    explicit Impl(library::MusicLibrary& ml)
      : ml{ml}
    {
    }

    Result<ImportReport> applyImportFromYaml(std::filesystem::path const& path, ImportMode mode, ImportRunMode runMode);

    void populateDeletionStats(ValidatedImport const& val, ImportReport& rep);
    Result<> clearDatabase(ValidatedImport const& val, lmdb::WriteTransaction& writeTransaction);

    Result<ValidatedImport> validate(ryml::ConstNodeRef const& root) const;
    Result<> validateTracks(ryml::ConstNodeRef const& tracks, ValidatedImport& validated) const;
    Result<> validateLists(ryml::ConstNodeRef const& lists, ValidatedImport& validated) const;

    Result<> importTracks(std::vector<ValidatedTrack> const& tracks,
                          lmdb::WriteTransaction& transaction,
                          std::unordered_map<std::uint32_t, TrackId>& yamlTrackIdToInternalId,
                          ImportMode strategy,
                          ExportMode payloadMode,
                          ImportReport& report);
    Result<> importTrackRecord(ValidatedTrack const& validatedTrack,
                               lmdb::WriteTransaction& transaction,
                               library::TrackStore::Writer& trackWriter,
                               library::FileManifestStore::Writer& manifestWriter,
                               library::FileManifestStore::Reader const& manifestReader,
                               ImportMode strategy,
                               ExportMode payloadMode,
                               std::unordered_map<std::uint32_t, TrackId>& yamlTrackIdToInternalId,
                               ImportReport& report);
    Result<> loadTrackBaseline(std::string_view uriStr,
                               std::optional<TrackId> const& optExistingTrackId,
                               ExportMode payloadMode,
                               std::optional<library::TrackBuilder>& optBuilder,
                               std::unique_ptr<tag::TagFile>& keepAliveTagFilePtr,
                               library::TrackStore::Writer& trackWriter);

    Result<> importLists(std::vector<ValidatedList> const& lists,
                         lmdb::WriteTransaction& transaction,
                         std::unordered_map<std::uint32_t, TrackId> const& yamlTrackIdToInternalId,
                         ImportMode strategy,
                         ImportReport& report);

    Result<std::vector<std::vector<std::byte>>> importCovers(ryml::ConstNodeRef const& trackNode,
                                                             library::TrackBuilder& builder) const;
    Result<> applyFileMetadata(ryml::ConstNodeRef const& trackNode,
                               std::string_view uriStr,
                               library::FileManifestStore::Reader const& manifestReader,
                               library::FileManifestBuilder& manifestBuilder) const;
    Result<TrackId> writePreparedTrackRecord(library::TrackStore::Writer& trackWriter,
                                             std::optional<TrackId> const& optExistingTrackId,
                                             library::TrackBuilder::PreparedHot const& preparedHot,
                                             library::TrackBuilder::PreparedCold const& preparedCold) const;

    void buildStaticListTracks(library::ListBuilder& builder,
                               ValidatedList const& importedList,
                               std::unordered_map<std::uint32_t, TrackId> const& yamlTrackIdToInternalId) const;
    void buildStaticListUris(library::ListBuilder& builder,
                             ValidatedList const& importedList,
                             library::FileManifestStore::Reader const& manifestReader) const;
    Result<> updateListParent(ValidatedList const& importedList,
                              std::unordered_map<std::uint32_t, ListId> const& yamlListIdToNewListId,
                              library::ListStore::Writer& listWriter) const;

    Result<> overlayMetadata(library::TrackBuilder& builder, ryml::ConstNodeRef const& trackNode) const;
    Result<> overlayTagsAndCustomMetadata(library::TrackBuilder& builder, ryml::ConstNodeRef const& trackNode) const;
    Result<> overlayTechnicalProperties(library::TrackBuilder& builder, ryml::ConstNodeRef const& trackNode) const;

    Result<> loadFileBaseline(std::string_view uriStr,
                              ExportMode payloadMode,
                              std::optional<library::TrackBuilder>& optBuilder,
                              std::unique_ptr<tag::TagFile>& keepAliveTagFilePtr) const;

    library::MusicLibrary& ml;
  };

  LibraryYamlImporter::LibraryYamlImporter(library::MusicLibrary& ml)
    : _implPtr{std::make_unique<Impl>(ml)}
  {
  }

  LibraryYamlImporter::~LibraryYamlImporter() = default;

  Result<ImportReport> LibraryYamlImporter::importFromYaml(std::filesystem::path const& path, ImportMode mode)
  {
    return _implPtr->applyImportFromYaml(path, mode, ImportRunMode::Commit);
  }

  Result<ImportReport> LibraryYamlImporter::previewImportFromYaml(std::filesystem::path const& path, ImportMode mode)
  {
    return _implPtr->applyImportFromYaml(path, mode, ImportRunMode::Preview);
  }

  Result<ImportReport> LibraryYamlImporter::Impl::applyImportFromYaml(std::filesystem::path const& path,
                                                                      ImportMode mode,
                                                                      ImportRunMode runMode)
  {
    auto buffer = std::vector<char>{};
    auto yamlContext = yaml::CallbackContext{path.string()};
    auto tree = ryml::Tree{};

    auto bufferResult = yaml::readFileResult(path);

    if (!bufferResult)
    {
      return makeError(
        Error::Code::IoError, std::format("Failed to read '{}': {}", path.string(), bufferResult.error().message));
    }

    buffer = std::move(*bufferResult);

    try
    {
      tree = ryml::Tree{yaml::callbacks(yamlContext)};
      yaml::parseInPlace(tree, buffer, yamlContext);
      tree.resolve();
    }
    catch (std::exception const& e)
    {
      return makeError(Error::Code::FormatRejected, std::format("Failed to parse '{}': {}", path.string(), e.what()));
    }

    auto const validationResult = validate(tree.rootref());

    if (!validationResult)
    {
      return std::unexpected{validationResult.error()};
    }

    auto const& validated = *validationResult;
    auto report = ImportReport{};

    if (mode == ImportMode::Restore)
    {
      populateDeletionStats(validated, report);
    }

    auto transaction = ml.writeTransaction();

    if (mode == ImportMode::Restore)
    {
      if (auto const clearResult = clearDatabase(validated, transaction); !clearResult)
      {
        return std::unexpected{clearResult.error()};
      }
    }

    auto yamlTrackIdToInternalId = std::unordered_map<std::uint32_t, TrackId>{};

    if (!validated.tracks.empty())
    {
      if (auto result =
            importTracks(validated.tracks, transaction, yamlTrackIdToInternalId, mode, validated.payloadMode, report);
          !result)
      {
        return std::unexpected{result.error()};
      }
    }

    if (!validated.lists.empty())
    {
      if (auto result = importLists(validated.lists, transaction, yamlTrackIdToInternalId, mode, report); !result)
      {
        return std::unexpected{result.error()};
      }
    }

    if (runMode == ImportRunMode::Preview)
    {
      return report;
    }

    if (auto result = transaction.commit(); !result)
    {
      return std::unexpected{result.error()};
    }

    if (mode == ImportMode::Restore && validated.optLibraryId)
    {
      auto header = ml.metadataHeader();
      header.libraryId = parseUuid(*validated.optLibraryId);
      ml.updateMetadataHeader(header);
    }

    return report;
  }

  void LibraryYamlImporter::Impl::populateDeletionStats(ValidatedImport const& val, ImportReport& rep)
  {
    auto readTransaction = ml.readTransaction();

    if (val.payloadMode != ExportMode::ListOnly)
    {
      for ([[maybe_unused]] auto const& row : ml.tracks().reader(readTransaction))
      {
        ++rep.tracksDeleted;
      }
    }

    for ([[maybe_unused]] auto const& row : ml.lists().reader(readTransaction))
    {
      ++rep.listsDeleted;
    }
  }

  Result<> LibraryYamlImporter::Impl::clearDatabase(ValidatedImport const& val,
                                                    lmdb::WriteTransaction& writeTransaction)
  {
    if (val.payloadMode != ExportMode::ListOnly)
    {
      if (auto result = ml.tracks().writer(writeTransaction).clear(); !result)
      {
        return result;
      }

      if (auto result = ml.manifest().writer(writeTransaction).clear(); !result)
      {
        return result;
      }
    }

    return ml.lists().writer(writeTransaction).clear();
  }

  // NOLINTNEXTLINE(readability-function-cognitive-complexity)
  Result<ValidatedImport> LibraryYamlImporter::Impl::validate(ryml::ConstNodeRef const& root) const
  {
    auto validated = ValidatedImport{};

    if (auto result = requireMap(root, "YAML root"); !result)
    {
      return std::unexpected{result.error()};
    }

    auto const versionNode = yaml::findChild(root, "version");

    if (!versionNode.readable())
    {
      return makeError(Error::Code::FormatRejected, "Missing 'version' field in YAML");
    }

    auto versionResult = requireScalarAs<std::uint32_t>(versionNode, "version");

    if (!versionResult)
    {
      return std::unexpected{versionResult.error()};
    }

    validated.version = *versionResult;

    if (validated.version != 1)
    {
      return makeError(Error::Code::FormatRejected, std::format("Unsupported YAML version {}", validated.version));
    }

    if (auto const exportModeNode = yaml::findChild(root, "export_mode"); exportModeNode.readable())
    {
      auto exportModeText = requireScalar(exportModeNode, "export_mode");

      if (!exportModeText)
      {
        return std::unexpected{exportModeText.error()};
      }

      auto optExportMode = parseExportMode(*exportModeText);

      if (!optExportMode)
      {
        return makeError(Error::Code::FormatRejected, std::format("Unknown export_mode '{}'", *exportModeText));
      }

      validated.payloadMode = *optExportMode;
    }
    else
    {
      validated.payloadMode = ExportMode::Full;
    }

    if (auto const libraryIdNode = yaml::findChild(root, "libraryId"); libraryIdNode.readable())
    {
      auto libraryId = requireScalar(libraryIdNode, "libraryId");

      if (!libraryId)
      {
        return std::unexpected{libraryId.error()};
      }

      if (!isUuidText(*libraryId))
      {
        return makeError(Error::Code::FormatRejected, "libraryId must be a UUID");
      }

      validated.optLibraryId = *libraryId;
    }

    auto const library = yaml::findChild(root, "library");

    if (!library.readable())
    {
      return makeError(Error::Code::FormatRejected, "Missing 'library' section in YAML");
    }

    if (auto result = requireMap(library, "library"); !result)
    {
      return std::unexpected{result.error()};
    }

    if (validated.payloadMode != ExportMode::ListOnly)
    {
      if (auto const tracks = yaml::findChild(library, "tracks"); tracks.readable())
      {
        if (auto result = requireSequence(tracks, "library.tracks"); !result)
        {
          return std::unexpected{result.error()};
        }

        if (auto const result = validateTracks(tracks, validated); !result)
        {
          return std::unexpected{result.error()};
        }
      }
    }

    if (auto const lists = yaml::findChild(library, "lists"); lists.readable())
    {
      if (auto result = requireSequence(lists, "library.lists"); !result)
      {
        return std::unexpected{result.error()};
      }

      if (auto const result = validateLists(lists, validated); !result)
      {
        return std::unexpected{result.error()};
      }
    }

    return validated;
  }

  Result<> LibraryYamlImporter::Impl::validateTracks(ryml::ConstNodeRef const& tracks, ValidatedImport& validated) const
  {
    auto seenYamlIds = std::unordered_set<std::uint32_t>{};

    for (auto const& trackNode : tracks.children())
    {
      if (auto result = requireMap(trackNode, "Track record"); !result)
      {
        return std::unexpected{result.error()};
      }

      auto track = ValidatedTrack{};
      auto uri = requireScalarField(trackNode, "uri", "Track record");

      if (!uri)
      {
        return std::unexpected{uri.error()};
      }

      track.uri = normalizedUri(*uri);

      if (track.uri.empty())
      {
        return makeError(Error::Code::FormatRejected, "Track record has empty 'uri'");
      }

      if (auto const idNode = yaml::findChild(trackNode, "id"); idNode.readable())
      {
        auto yamlId = requireScalarAs<std::uint32_t>(idNode, "Track record.id");

        if (!yamlId)
        {
          return std::unexpected{yamlId.error()};
        }

        track.yamlId = *yamlId;

        if (track.yamlId != 0)
        {
          if (seenYamlIds.contains(track.yamlId))
          {
            return makeError(
              Error::Code::FormatRejected, std::format("Duplicate track YAML id {} in payload", track.yamlId));
          }

          seenYamlIds.insert(track.yamlId);
        }
      }

      track.node = trackNode;
      validated.tracks.push_back(std::move(track));
    }

    return {};
  }

  // NOLINTNEXTLINE(readability-function-cognitive-complexity)
  Result<> LibraryYamlImporter::Impl::validateLists(ryml::ConstNodeRef const& lists, ValidatedImport& validated) const
  {
    auto seenYamlIds = std::unordered_set<std::uint32_t>{};

    for (auto const& listNode : lists.children())
    {
      if (auto result = requireMap(listNode, "List record"); !result)
      {
        return std::unexpected{result.error()};
      }

      auto list = ValidatedList{};
      auto yamlId = requireScalarFieldAs<std::uint32_t>(listNode, "id", "List record");

      if (!yamlId)
      {
        return std::unexpected{yamlId.error()};
      }

      list.yamlId = *yamlId;

      if (list.yamlId == 0)
      {
        return makeError(Error::Code::FormatRejected, "List id 0 is reserved for the root");
      }

      if (seenYamlIds.contains(list.yamlId))
      {
        return makeError(Error::Code::FormatRejected, std::format("Duplicate list YAML id {} in payload", list.yamlId));
      }

      seenYamlIds.insert(list.yamlId);

      auto name = requireScalarField(listNode, "name", "List record");

      if (!name)
      {
        return std::unexpected{name.error()};
      }

      list.name = *name;

      if (auto const parentIdNode = yaml::findChild(listNode, "parentId"); parentIdNode.readable())
      {
        auto parentId = requireScalarAs<std::uint32_t>(parentIdNode, "List record.parentId");

        if (!parentId)
        {
          return std::unexpected{parentId.error()};
        }

        list.yamlParentId = *parentId;
      }

      if (auto const descriptionNode = yaml::findChild(listNode, "description"); descriptionNode.readable())
      {
        auto description = requireScalar(descriptionNode, "List record.description");

        if (!description)
        {
          return std::unexpected{description.error()};
        }

        list.description = *description;
      }

      if (auto const filterNode = yaml::findChild(listNode, "filter"); filterNode.readable())
      {
        auto filter = requireScalar(filterNode, "List record.filter");

        if (!filter)
        {
          return std::unexpected{filter.error()};
        }

        list.isSmart = true;
        list.filter = *filter;
      }
      else if (auto const tracksNode = yaml::findChild(listNode, "tracks"); tracksNode.readable())
      {
        if (auto result = requireSequence(tracksNode, "List record.tracks"); !result)
        {
          return std::unexpected{result.error()};
        }

        for (auto const& trackRef : tracksNode.children())
        {
          if (trackRef.is_val())
          {
            auto trackId = requireScalarAs<std::uint32_t>(trackRef, "List record.tracks[]");

            if (!trackId)
            {
              return std::unexpected{trackId.error()};
            }

            list.yamlTrackIds.push_back(*trackId);
          }
          else if (trackRef.is_map())
          {
            if (auto const trackIdNode = yaml::findChild(trackRef, "id"); trackIdNode.readable())
            {
              auto trackId = requireScalarAs<std::uint32_t>(trackIdNode, "List record.tracks[].id");

              if (!trackId)
              {
                return std::unexpected{trackId.error()};
              }

              list.yamlTrackIds.push_back(*trackId);
            }
            else if (auto const uriNode = yaml::findChild(trackRef, "uri"); uriNode.readable())
            {
              auto uri = requireScalar(uriNode, "List record.tracks[].uri");

              if (!uri)
              {
                return std::unexpected{uri.error()};
              }

              auto normalized = normalizedUri(*uri);

              if (normalized.empty())
              {
                return makeError(Error::Code::FormatRejected, "List track reference has empty 'uri'");
              }

              list.trackUris.push_back(std::move(normalized));
            }
            else
            {
              return makeError(Error::Code::FormatRejected, "List track reference missing 'id' or 'uri'");
            }
          }
          else
          {
            return makeError(Error::Code::FormatRejected, "List track reference must be a scalar or map");
          }
        }
      }

      validated.lists.push_back(std::move(list));
    }

    return {};
  }

  Result<> LibraryYamlImporter::Impl::importTracks(std::vector<ValidatedTrack> const& tracks,
                                                   lmdb::WriteTransaction& transaction,
                                                   std::unordered_map<std::uint32_t, TrackId>& yamlTrackIdToInternalId,
                                                   ImportMode strategy,
                                                   ExportMode payloadMode,
                                                   ImportReport& report)
  {
    auto trackWriter = ml.tracks().writer(transaction);
    auto manifestWriter = ml.manifest().writer(transaction);
    auto manifestReader = ml.manifest().reader(transaction);

    for (auto const& validatedTrack : tracks)
    {
      if (auto result = importTrackRecord(validatedTrack,
                                          transaction,
                                          trackWriter,
                                          manifestWriter,
                                          manifestReader,
                                          strategy,
                                          payloadMode,
                                          yamlTrackIdToInternalId,
                                          report);
          !result)
      {
        return std::unexpected{result.error()};
      }
    }

    return {};
  }

  Result<> LibraryYamlImporter::Impl::importTrackRecord(
    ValidatedTrack const& validatedTrack,
    lmdb::WriteTransaction& transaction,
    library::TrackStore::Writer& trackWriter,
    library::FileManifestStore::Writer& manifestWriter,
    library::FileManifestStore::Reader const& manifestReader,
    ImportMode strategy,
    ExportMode payloadMode,
    std::unordered_map<std::uint32_t, TrackId>& yamlTrackIdToInternalId,
    ImportReport& report)
  {
    auto& dictionary = ml.dictionary();
    auto& resources = ml.resources();

    auto const& trackNode = validatedTrack.node;
    std::string_view const& uriStr = validatedTrack.uri;

    auto optExistingTrackId = std::optional<TrackId>{};

    if (strategy == ImportMode::Merge)
    {
      if (auto const manifestResult = manifestReader.get(uriStr); manifestResult)
      {
        optExistingTrackId = manifestResult->trackId();
      }
    }

    auto optBuilder = std::optional<library::TrackBuilder>{};
    auto keepAliveTagFilePtr = std::unique_ptr<tag::TagFile>{};

    if (auto result =
          loadTrackBaseline(uriStr, optExistingTrackId, payloadMode, optBuilder, keepAliveTagFilePtr, trackWriter);
        !result)
    {
      return std::unexpected{result.error()};
    }

    auto builder = optBuilder ? *optBuilder : library::TrackBuilder::makeEmpty();

    if (!optBuilder)
    {
      builder.property().uri(uriStr);
    }

    if (auto result = overlayMetadata(builder, trackNode); !result)
    {
      return std::unexpected{result.error()};
    }

    if (auto result = overlayTagsAndCustomMetadata(builder, trackNode); !result)
    {
      return std::unexpected{result.error()};
    }

    if (auto result = overlayTechnicalProperties(builder, trackNode); !result)
    {
      return std::unexpected{result.error()};
    }

    auto decodedCoverBlobsResult = importCovers(trackNode, builder);

    if (!decodedCoverBlobsResult)
    {
      return std::unexpected{decodedCoverBlobsResult.error()};
    }

    auto decodedCoverBlobs = std::move(*decodedCoverBlobsResult);

    auto preparedResult = builder.prepare(transaction, dictionary, resources);

    if (!preparedResult)
    {
      return std::unexpected{preparedResult.error()};
    }

    auto& [preparedHot, preparedCold] = *preparedResult;

    auto targetTrackIdResult = writePreparedTrackRecord(trackWriter, optExistingTrackId, preparedHot, preparedCold);

    if (!targetTrackIdResult)
    {
      return std::unexpected{targetTrackIdResult.error()};
    }

    auto const targetTrackId = *targetTrackIdResult;

    if (optExistingTrackId)
    {
      ++report.tracksUpdated;
    }
    else
    {
      ++report.tracksCreated;
    }

    auto manifestBuilder = library::FileManifestBuilder::makeEmpty();
    manifestBuilder.trackId(targetTrackId);

    if (auto result = applyFileMetadata(trackNode, uriStr, manifestReader, manifestBuilder); !result)
    {
      return std::unexpected{result.error()};
    }

    if (auto putResult = manifestWriter.put(uriStr, manifestBuilder.serialize()); !putResult)
    {
      return std::unexpected{putResult.error()};
    }

    if (validatedTrack.yamlId != 0)
    {
      yamlTrackIdToInternalId[validatedTrack.yamlId] = targetTrackId;
    }

    return {};
  }

  Result<std::vector<std::vector<std::byte>>> LibraryYamlImporter::Impl::importCovers(
    ryml::ConstNodeRef const& trackNode,
    library::TrackBuilder& builder) const
  {
    auto decodedCoverBlobs = std::vector<std::vector<std::byte>>{};

    if (auto const coversNode = yaml::findChild(trackNode, "covers"); coversNode.readable() && coversNode.is_seq())
    {
      builder.coverArt().clear();
      decodedCoverBlobs.reserve(coversNode.num_children());

      for (auto const coverNode : coversNode)
      {
        if (auto result = requireMap(coverNode, "Track cover"); !result)
        {
          return std::unexpected{result.error()};
        }

        auto rawType = requireScalarFieldAs<std::uint32_t>(coverNode, "type", "Track cover");

        if (!rawType)
        {
          return std::unexpected{rawType.error()};
        }

        auto data = requireScalarField(coverNode, "data", "Track cover");

        if (!data)
        {
          return std::unexpected{data.error()};
        }

        auto const picType = *rawType <= static_cast<std::uint32_t>(library::PictureType::PublisherLogo)
                               ? static_cast<library::PictureType>(*rawType)
                               : library::PictureType::Other;

        // Keep the borrowed blob alive in decodedCoverBlobs until the builder serializes below.
        if (auto optDecoded = utility::base64Decode(*data); optDecoded && !optDecoded->empty())
        {
          decodedCoverBlobs.push_back(*std::move(optDecoded));
          builder.coverArt().add(picType, decodedCoverBlobs.back());
        }
        else
        {
          return makeError(Error::Code::FormatRejected, "Track cover data must be non-empty base64");
        }
      }
    }
    else if (auto const coversNode = yaml::findChild(trackNode, "covers"); coversNode.readable())
    {
      return makeError(Error::Code::FormatRejected, "Track covers must be a sequence");
    }

    return decodedCoverBlobs;
  }

  Result<> LibraryYamlImporter::Impl::applyFileMetadata(ryml::ConstNodeRef const& trackNode,
                                                        std::string_view uriStr,
                                                        library::FileManifestStore::Reader const& manifestReader,
                                                        library::FileManifestBuilder& manifestBuilder) const
  {
    if (auto const manifestResult = manifestReader.get(uriStr); manifestResult)
    {
      manifestBuilder.fileSize(manifestResult->fileSize());
      manifestBuilder.mtime(manifestResult->mtime());
    }
    else
    {
      auto fileEc = std::error_code{};

      if (auto const fullPath = ml.rootPath() / uriStr; std::filesystem::exists(fullPath, fileEc) && !fileEc)
      {
        auto const fileSize = std::filesystem::file_size(fullPath, fileEc);

        if (fileEc)
        {
          return makeError(Error::Code::IoError,
                           std::format("Failed to read file size for '{}': {}", fullPath.string(), fileEc.message()));
        }

        auto const lastWriteTime = std::filesystem::last_write_time(fullPath, fileEc);

        if (fileEc)
        {
          return makeError(
            Error::Code::IoError,
            std::format("Failed to read modification time for '{}': {}", fullPath.string(), fileEc.message()));
        }

        manifestBuilder.fileSize(fileSize);
        manifestBuilder.mtime(static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(lastWriteTime.time_since_epoch()).count()));
      }
      else if (fileEc)
      {
        return makeError(
          Error::Code::IoError, std::format("Failed to inspect file '{}': {}", fullPath.string(), fileEc.message()));
      }
    }

    if (auto fileSizeNode = yaml::findChild(trackNode, "fileSize"); fileSizeNode.readable())
    {
      auto fileSize = requireScalarAs<std::uint64_t>(fileSizeNode, "Track record.fileSize");

      if (!fileSize)
      {
        return std::unexpected{fileSize.error()};
      }

      manifestBuilder.fileSize(*fileSize);
    }

    if (auto mtimeNode = yaml::findChild(trackNode, "mtime"); mtimeNode.readable())
    {
      auto mtime = requireScalarAs<std::uint64_t>(mtimeNode, "Track record.mtime");

      if (!mtime)
      {
        return std::unexpected{mtime.error()};
      }

      manifestBuilder.mtime(*mtime);
    }

    return {};
  }

  Result<TrackId> LibraryYamlImporter::Impl::writePreparedTrackRecord(
    library::TrackStore::Writer& trackWriter,
    std::optional<TrackId> const& optExistingTrackId,
    library::TrackBuilder::PreparedHot const& preparedHot,
    library::TrackBuilder::PreparedCold const& preparedCold) const
  {
    if (optExistingTrackId)
    {
      auto const targetTrackId = *optExistingTrackId;
      auto writeResult = library::updatePreparedTrackRecord(trackWriter, targetTrackId, preparedHot, preparedCold);

      if (!writeResult)
      {
        return std::unexpected{writeResult.error()};
      }

      return targetTrackId;
    }

    auto createResult = library::createPreparedTrackRecord(trackWriter, preparedHot, preparedCold);

    if (!createResult)
    {
      return std::unexpected{createResult.error()};
    }

    auto const [newTrackId, view] = *createResult;
    return newTrackId;
  }

  Result<> LibraryYamlImporter::Impl::loadTrackBaseline(std::string_view uriStr,
                                                        std::optional<TrackId> const& optExistingTrackId,
                                                        ExportMode payloadMode,
                                                        std::optional<library::TrackBuilder>& optBuilder,
                                                        std::unique_ptr<tag::TagFile>& keepAliveTagFilePtr,
                                                        library::TrackStore::Writer& trackWriter)
  {
    if (optExistingTrackId)
    {
      if (auto optView = trackWriter.get(*optExistingTrackId, library::TrackStore::Reader::LoadMode::Both); optView)
      {
        optBuilder = library::TrackBuilder::fromView(*optView, ml.dictionary());
      }
    }

    if (payloadMode == ExportMode::Delta || payloadMode == ExportMode::Metadata)
    {
      if (auto result = loadFileBaseline(uriStr, payloadMode, optBuilder, keepAliveTagFilePtr); !result)
      {
        return std::unexpected{result.error()};
      }
    }

    return {};
  }

  Result<> LibraryYamlImporter::Impl::loadFileBaseline(std::string_view uriStr,
                                                       ExportMode payloadMode,
                                                       std::optional<library::TrackBuilder>& optBuilder,
                                                       std::unique_ptr<tag::TagFile>& keepAliveTagFilePtr) const
  {
    auto fileEc = std::error_code{};
    auto const fullPath = ml.rootPath() / uriStr;
    auto const fileExists = std::filesystem::exists(fullPath, fileEc);

    if (fileEc)
    {
      return makeError(
        Error::Code::IoError, std::format("Failed to inspect file '{}': {}", fullPath.string(), fileEc.message()));
    }

    if (!fileExists)
    {
      return {};
    }

    auto tagFileResult = tag::TagFile::open(fullPath);

    if (!tagFileResult)
    {
      return {};
    }

    keepAliveTagFilePtr = std::move(*tagFileResult);
    auto fileBuilderResult = keepAliveTagFilePtr->loadTrack();

    if (!fileBuilderResult)
    {
      return {};
    }

    if (!optBuilder)
    {
      optBuilder = std::move(*fileBuilderResult);

      if (payloadMode == ExportMode::Metadata)
      {
        optBuilder->metadata()
          .title("")
          .artist("")
          .album("")
          .albumArtist("")
          .composer("")
          .conductor("")
          .ensemble("")
          .genre("")
          .work("")
          .movement("")
          .soloist("")
          .year(0)
          .trackNumber(0)
          .trackTotal(0)
          .discNumber(0)
          .discTotal(0)
          .movementNumber(0)
          .movementTotal(0);
        optBuilder->tags().clear();
        optBuilder->customMetadata().clear();
      }

      optBuilder->property().uri(uriStr);
      return {};
    }

    auto const& fileBuilder = *fileBuilderResult;
    auto const& props = fileBuilder.property();
    optBuilder->property()
      .duration(props.duration())
      .bitrate(props.bitrate())
      .sampleRate(props.sampleRate())
      .codec(props.codec())
      .channels(props.channels())
      .bitDepth(props.bitDepth());

    if (payloadMode == ExportMode::Delta && optBuilder->coverArt().entries().empty())
    {
      for (auto const& pending : fileBuilder.coverArt().entries())
      {
        std::visit([&](auto source) { optBuilder->coverArt().add(pending.type, source); }, pending.source);
      }
    }

    optBuilder->property().uri(uriStr);
    return {};
  }

  Result<> LibraryYamlImporter::Impl::overlayMetadata(library::TrackBuilder& builder,
                                                      ryml::ConstNodeRef const& trackNode) const
  {
    using StringSetter = void (*)(library::TrackBuilder::MetadataBuilder&, std::string_view);
    using NumberSetter = void (*)(library::TrackBuilder::MetadataBuilder&, std::uint16_t);

    struct Dispatch
    {
      rt::TrackField field;
      StringSetter stringSetter = nullptr;
      NumberSetter numberSetter = nullptr;
    };

    constexpr auto kMetadataDispatch = std::to_array<Dispatch>({
      {.field = rt::TrackField::Title, .stringSetter = [](auto& metadata, auto value) { metadata.title(value); }},
      {.field = rt::TrackField::Artist, .stringSetter = [](auto& metadata, auto value) { metadata.artist(value); }},
      {.field = rt::TrackField::Album, .stringSetter = [](auto& metadata, auto value) { metadata.album(value); }},
      {.field = rt::TrackField::AlbumArtist,
       .stringSetter = [](auto& metadata, auto value) { metadata.albumArtist(value); }},
      {.field = rt::TrackField::Composer, .stringSetter = [](auto& metadata, auto value) { metadata.composer(value); }},
      {.field = rt::TrackField::Conductor,
       .stringSetter = [](auto& metadata, auto value) { metadata.conductor(value); }},
      {.field = rt::TrackField::Ensemble, .stringSetter = [](auto& metadata, auto value) { metadata.ensemble(value); }},
      {.field = rt::TrackField::Genre, .stringSetter = [](auto& metadata, auto value) { metadata.genre(value); }},
      {.field = rt::TrackField::Work, .stringSetter = [](auto& metadata, auto value) { metadata.work(value); }},
      {.field = rt::TrackField::Movement, .stringSetter = [](auto& metadata, auto value) { metadata.movement(value); }},
      {.field = rt::TrackField::Soloist, .stringSetter = [](auto& metadata, auto value) { metadata.soloist(value); }},
      {.field = rt::TrackField::Year, .numberSetter = [](auto& metadata, auto value) { metadata.year(value); }},
      {.field = rt::TrackField::TrackNumber,
       .numberSetter = [](auto& metadata, auto value) { metadata.trackNumber(value); }},
      {.field = rt::TrackField::TrackTotal,
       .numberSetter = [](auto& metadata, auto value) { metadata.trackTotal(value); }},
      {.field = rt::TrackField::DiscNumber,
       .numberSetter = [](auto& metadata, auto value) { metadata.discNumber(value); }},
      {.field = rt::TrackField::DiscTotal,
       .numberSetter = [](auto& metadata, auto value) { metadata.discTotal(value); }},
      {.field = rt::TrackField::MovementNumber,
       .numberSetter = [](auto& metadata, auto value) { metadata.movementNumber(value); }},
      {.field = rt::TrackField::MovementTotal,
       .numberSetter = [](auto& metadata, auto value) { metadata.movementTotal(value); }},
    });

    for (auto const& map : kMetadataDispatch)
    {
      auto const key = rt::trackFieldId(map.field);

      if (auto node = yaml::findChild(trackNode, key); node.readable())
      {
        if (map.stringSetter != nullptr)
        {
          auto text = requireScalarInFieldContext(node, "Track record", key);

          if (!text)
          {
            return std::unexpected{text.error()};
          }

          map.stringSetter(builder.metadata(), *text);
        }
        else if (map.numberSetter != nullptr)
        {
          auto value = requireScalarInFieldContextAs<std::uint16_t>(node, "Track record", key);

          if (!value)
          {
            return std::unexpected{value.error()};
          }

          map.numberSetter(builder.metadata(), *value);
        }
      }
    }

    return {};
  }

  Result<> LibraryYamlImporter::Impl::overlayTagsAndCustomMetadata(library::TrackBuilder& builder,
                                                                   ryml::ConstNodeRef const& trackNode) const
  {
    if (auto tagsNode = yaml::findChild(trackNode, "tags"); tagsNode.readable())
    {
      if (auto result = requireSequence(tagsNode, "Track record.tags"); !result)
      {
        return std::unexpected{result.error()};
      }

      builder.tags().clear();

      for (auto const& tag : tagsNode.children())
      {
        auto text = requireScalar(tag, "Track record.tags[]");

        if (!text)
        {
          return std::unexpected{text.error()};
        }

        builder.tags().add(*text);
      }
    }

    if (auto customNode = yaml::findChild(trackNode, "custom"); customNode.readable())
    {
      if (auto result = requireMap(customNode, "Track record.custom"); !result)
      {
        return std::unexpected{result.error()};
      }

      builder.customMetadata().clear();

      for (auto const& it : customNode.children())
      {
        auto value = requireScalar(it, std::format("Track record.custom.{}", yaml::keyView(it)));

        if (!value)
        {
          return std::unexpected{value.error()};
        }

        builder.customMetadata().add(yaml::keyView(it), *value);
      }
    }

    return {};
  }

  Result<> LibraryYamlImporter::Impl::overlayTechnicalProperties(library::TrackBuilder& builder,
                                                                 ryml::ConstNodeRef const& trackNode) const
  {
    using U32Setter = void (*)(library::TrackBuilder::PropertyBuilder&, std::uint32_t);
    using U16Setter = void (*)(library::TrackBuilder::PropertyBuilder&, std::uint16_t);
    using U8Setter = void (*)(library::TrackBuilder::PropertyBuilder&, std::uint8_t);

    struct Dispatch
    {
      rt::TrackField field;
      U32Setter u32Setter = nullptr;
      U16Setter u16Setter = nullptr;
      U8Setter u8Setter = nullptr;
    };

    constexpr auto kPropertyDispatch = std::to_array<Dispatch>({
      {.field = rt::TrackField::Duration,
       .u32Setter = [](auto& prop, auto value) { prop.duration(std::chrono::milliseconds{value}); }},
      {.field = rt::TrackField::Bitrate, .u32Setter = [](auto& prop, auto value) { prop.bitrate(Bitrate{value}); }},
      {.field = rt::TrackField::SampleRate,
       .u32Setter = [](auto& prop, auto value) { prop.sampleRate(SampleRate{value}); }},
      {.field = rt::TrackField::Channels, .u8Setter = [](auto& prop, auto value) { prop.channels(Channels{value}); }},
      {.field = rt::TrackField::BitDepth, .u8Setter = [](auto& prop, auto value) { prop.bitDepth(BitDepth{value}); }},
    });

    if (auto codecNode = yaml::findChild(trackNode, rt::trackFieldId(rt::TrackField::Codec)); codecNode.readable())
    {
      auto codec = requireScalar(codecNode, "Track record.codec");

      if (!codec)
      {
        return std::unexpected{codec.error()};
      }

      if (auto const optCodec = parseAudioCodecName(*codec); optCodec)
      {
        builder.property().codec(*optCodec);
      }
    }

    for (auto const& map : kPropertyDispatch)
    {
      auto const key = rt::trackFieldId(map.field);

      if (auto node = yaml::findChild(trackNode, key); node.readable())
      {
        if (map.u32Setter != nullptr)
        {
          auto value = requireScalarInFieldContextAs<std::uint32_t>(node, "Track record", key);

          if (!value)
          {
            return std::unexpected{value.error()};
          }

          map.u32Setter(builder.property(), *value);
        }
        else if (map.u16Setter != nullptr)
        {
          auto value = requireScalarInFieldContextAs<std::uint16_t>(node, "Track record", key);

          if (!value)
          {
            return std::unexpected{value.error()};
          }

          map.u16Setter(builder.property(), *value);
        }
        else if (map.u8Setter != nullptr)
        {
          auto value = requireScalarInFieldContextAs<std::uint8_t>(node, "Track record", key);

          if (!value)
          {
            return std::unexpected{value.error()};
          }

          map.u8Setter(builder.property(), *value);
        }
      }
    }

    return {};
  }

  Result<> LibraryYamlImporter::Impl::importLists(
    std::vector<ValidatedList> const& lists,
    lmdb::WriteTransaction& transaction,
    std::unordered_map<std::uint32_t, TrackId> const& yamlTrackIdToInternalId,
    ImportMode /*strategy*/,
    ImportReport& report)
  {
    auto listWriter = ml.lists().writer(transaction);
    auto manifestReader = ml.manifest().reader(transaction);

    auto yamlListIdToNewListId = std::unordered_map<std::uint32_t, ListId>{};
    yamlListIdToNewListId.reserve(lists.size());

    for (auto const& importedList : lists)
    {
      auto builder = library::ListBuilder::makeEmpty().name(importedList.name).description(importedList.description);

      if (importedList.isSmart)
      {
        builder.filter(importedList.filter);
      }
      else
      {
        buildStaticListTracks(builder, importedList, yamlTrackIdToInternalId);
        buildStaticListUris(builder, importedList, manifestReader);
      }

      auto createResult = listWriter.create(builder.serialize());

      if (!createResult)
      {
        return std::unexpected{createResult.error()};
      }

      auto const [newListId, view] = *createResult;
      yamlListIdToNewListId[importedList.yamlId] = newListId;
      ++report.listsCreated;
    }

    for (auto const& importedList : lists)
    {
      if (auto result = updateListParent(importedList, yamlListIdToNewListId, listWriter); !result)
      {
        return std::unexpected{result.error()};
      }
    }

    return {};
  }

  void LibraryYamlImporter::Impl::buildStaticListTracks(
    library::ListBuilder& builder,
    ValidatedList const& importedList,
    std::unordered_map<std::uint32_t, TrackId> const& yamlTrackIdToInternalId) const
  {
    for (auto const yamlTrackId : importedList.yamlTrackIds)
    {
      if (auto const it = yamlTrackIdToInternalId.find(yamlTrackId); it != yamlTrackIdToInternalId.end())
      {
        builder.tracks().add(it->second);
      }
    }
  }

  void LibraryYamlImporter::Impl::buildStaticListUris(library::ListBuilder& builder,
                                                      ValidatedList const& importedList,
                                                      library::FileManifestStore::Reader const& manifestReader) const
  {
    for (auto const& uri : importedList.trackUris)
    {
      if (auto const manifestResult = manifestReader.get(uri); manifestResult)
      {
        builder.tracks().add(manifestResult->trackId());
      }
    }
  }

  Result<> LibraryYamlImporter::Impl::updateListParent(
    ValidatedList const& importedList,
    std::unordered_map<std::uint32_t, ListId> const& yamlListIdToNewListId,
    library::ListStore::Writer& listWriter) const
  {
    if (importedList.yamlParentId == 0)
    {
      return {};
    }

    auto const parentIt = yamlListIdToNewListId.find(importedList.yamlParentId);

    if (parentIt == yamlListIdToNewListId.end())
    {
      return {};
    }

    auto const childId = yamlListIdToNewListId.at(importedList.yamlId);
    auto optListView = listWriter.get(childId);

    if (!optListView)
    {
      return {};
    }

    auto builder = library::ListBuilder::fromView(*optListView).parentId(parentIt->second);

    if (auto result = listWriter.update(childId, builder.serialize()); !result)
    {
      return std::unexpected{result.error()};
    }

    return {};
  }
} // namespace ao::rt
