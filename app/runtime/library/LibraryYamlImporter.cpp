// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "LibraryYamlImportOperation.h"
#include "MediaTrack.h"
#include "TrackBuilderSnapshot.h"
#include <ao/AudioCodec.h>
#include <ao/AudioScalars.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/PictureType.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/LibraryUri.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListStore.h>
#include <ao/library/MetadataLayout.h>
#include <ao/library/MetadataStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackWrite.h>
#include <ao/library/WritableMusicLibrary.h>
#include <ao/query/Parser.h>
#include <ao/query/QueryCompiler.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryYamlExporter.h>
#include <ao/rt/library/LibraryYamlImporter.h>
#include <ao/utility/Base64.h>
#include <ao/yaml/RymlAdapter.h>

#include <boost/unordered/unordered_flat_set.hpp>
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
#include <functional>
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

    struct ValidatedListTrackIdReference final
    {
      std::uint32_t yamlId = 0;
    };

    struct ValidatedListTrackUriReference final
    {
      std::string uri;
    };

    using ValidatedListTrackReference = std::variant<ValidatedListTrackIdReference, ValidatedListTrackUriReference>;

    struct ValidatedList final
    {
      std::uint32_t yamlId = 0;
      std::uint32_t yamlParentId = 0;
      std::string_view name;
      std::string_view description;
      std::string_view filter;
      std::vector<ValidatedListTrackReference> trackReferences;
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

    struct PreparedTrack final
    {
      std::uint32_t yamlId = 0;
      std::string uri;
      std::optional<TrackId> optExistingTrackId;
      TrackBuilderSnapshot builder;
      library::FileManifestBuilder manifest;
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

    Result<std::string> parseLibraryUri(std::string_view uri, std::string_view context)
    {
      auto parsed = library::LibraryUri::parse(uri);

      if (!parsed)
      {
        return makeError(Error::Code::FormatRejected, std::format("{}: {}", context, parsed.error().message));
      }

      return std::string{parsed->value()};
    }

    Result<> rejectUnknownFields(ryml::ConstNodeRef const& node,
                                 std::span<std::string_view const> allowedFields,
                                 std::string_view context)
    {
      auto seenFields = std::unordered_set<std::string_view>{};

      for (auto const& child : node.children())
      {
        auto const key = yaml::keyView(child);

        if (!std::ranges::contains(allowedFields, key))
        {
          return makeError(Error::Code::FormatRejected, std::format("{} contains unknown field '{}'", context, key));
        }

        if (!seenFields.insert(key).second)
        {
          return makeError(Error::Code::FormatRejected, std::format("{} contains duplicate field '{}'", context, key));
        }
      }

      return {};
    }

    constexpr auto kRootFields = std::to_array<std::string_view>({"version", "libraryId", "export_mode", "library"});
    constexpr auto kLibraryFields = std::to_array<std::string_view>({"tracks", "lists"});
    constexpr auto kTrackFields = std::to_array<std::string_view>({
      "id",           "uri",         "title",       "artist",     "album",           "album-artist",   "genre",
      "composer",     "conductor",   "ensemble",    "work",       "movement",        "soloist",        "year",
      "track-number", "track-total", "disc-number", "disc-total", "movement-number", "movement-total", "custom",
      "tags",         "covers",      "duration",    "bitrate",    "sample-rate",     "codec",          "channels",
      "bit-depth",    "fileSize",    "mtime",
    });
    constexpr auto kCoverFields = std::to_array<std::string_view>({"type", "data"});
    constexpr auto kListFields =
      std::to_array<std::string_view>({"id", "parentId", "name", "description", "filter", "tracks"});
    constexpr auto kListReferenceFields = std::to_array<std::string_view>({"id", "uri"});

    Result<> validateTrackNestedSchema(ryml::ConstNodeRef const& trackNode)
    {
      if (auto const custom = yaml::findChild(trackNode, "custom"); custom.readable())
      {
        if (auto result = requireMap(custom, "Track record.custom"); !result)
        {
          return result;
        }

        auto keys = std::unordered_set<std::string_view>{};

        for (auto const& entry : custom.children())
        {
          if (auto const key = yaml::keyView(entry); !keys.insert(key).second)
          {
            return makeError(
              Error::Code::FormatRejected, std::format("Track record.custom contains duplicate field '{}'", key));
          }
        }
      }

      auto const covers = yaml::findChild(trackNode, "covers");

      if (!covers.readable())
      {
        return {};
      }

      if (auto result = requireSequence(covers, "Track record.covers"); !result)
      {
        return result;
      }

      for (auto const& cover : covers.children())
      {
        if (auto result = requireMap(cover, "Track cover"); !result)
        {
          return result;
        }

        if (auto result = rejectUnknownFields(cover, kCoverFields, "Track cover"); !result)
        {
          return result;
        }
      }

      return {};
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
      if (modeStr == "delta")
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

    Result<> validateListIdentity(ryml::ConstNodeRef const& listNode,
                                  std::unordered_set<std::uint32_t>& seenYamlIds,
                                  ValidatedList& list)
    {
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

      if (!seenYamlIds.insert(list.yamlId).second)
      {
        return makeError(Error::Code::FormatRejected, std::format("Duplicate list YAML id {} in payload", list.yamlId));
      }

      auto name = requireScalarField(listNode, "name", "List record");

      if (!name)
      {
        return std::unexpected{name.error()};
      }

      list.name = *name;
      return {};
    }

    Result<> validateListMetadata(ryml::ConstNodeRef const& listNode, ValidatedList& list)
    {
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

      return {};
    }

    Result<ValidatedListTrackReference> validateListTrackReference(ryml::ConstNodeRef const& trackRef)
    {
      if (trackRef.is_val())
      {
        auto trackId = requireScalarAs<std::uint32_t>(trackRef, "List record.tracks[]");

        if (!trackId)
        {
          return std::unexpected{trackId.error()};
        }

        return ValidatedListTrackIdReference{.yamlId = *trackId};
      }

      if (!trackRef.is_map())
      {
        return makeError(Error::Code::FormatRejected, "List track reference must be a scalar or map");
      }

      if (auto result = rejectUnknownFields(trackRef, kListReferenceFields, "List track reference"); !result)
      {
        return std::unexpected{result.error()};
      }

      auto const trackIdNode = yaml::findChild(trackRef, "id");
      auto const uriNode = yaml::findChild(trackRef, "uri");

      if (trackIdNode.readable() == uriNode.readable())
      {
        return makeError(Error::Code::FormatRejected, "List track reference must contain exactly one of 'id' or 'uri'");
      }

      if (trackIdNode.readable())
      {
        auto trackId = requireScalarAs<std::uint32_t>(trackIdNode, "List record.tracks[].id");

        if (!trackId)
        {
          return std::unexpected{trackId.error()};
        }

        return ValidatedListTrackIdReference{.yamlId = *trackId};
      }

      auto uri = requireScalar(uriNode, "List record.tracks[].uri");

      if (!uri)
      {
        return std::unexpected{uri.error()};
      }

      auto normalized = parseLibraryUri(*uri, "List record.tracks[].uri");

      if (!normalized)
      {
        return std::unexpected{normalized.error()};
      }

      return ValidatedListTrackUriReference{.uri = std::move(*normalized)};
    }

    Result<> validateManualListTracks(ryml::ConstNodeRef const& tracksNode, ValidatedList& list)
    {
      if (auto result = requireSequence(tracksNode, "List record.tracks"); !result)
      {
        return std::unexpected{result.error()};
      }

      for (auto const& trackRef : tracksNode.children())
      {
        auto reference = validateListTrackReference(trackRef);

        if (!reference)
        {
          return std::unexpected{reference.error()};
        }

        list.trackReferences.push_back(std::move(*reference));
      }

      return {};
    }

    Result<> validateListContents(ryml::ConstNodeRef const& listNode, ValidatedList& list)
    {
      auto const filterNode = yaml::findChild(listNode, "filter");
      auto const tracksNode = yaml::findChild(listNode, "tracks");

      if (filterNode.readable() && tracksNode.readable())
      {
        return makeError(Error::Code::FormatRejected, "List record cannot contain both 'filter' and 'tracks'");
      }

      if (filterNode.readable())
      {
        auto filter = requireScalar(filterNode, "List record.filter");

        if (!filter)
        {
          return std::unexpected{filter.error()};
        }

        if (filter->empty())
        {
          return makeError(Error::Code::FormatRejected, "List record.filter must be non-empty");
        }

        auto expression = query::parse(*filter);

        if (!expression)
        {
          return makeError(
            Error::Code::FormatRejected, std::format("List record.filter is invalid: {}", expression.error().message));
        }

        if (auto plan = query::compileQuery(*expression); !plan)
        {
          return makeError(
            Error::Code::FormatRejected, std::format("List record.filter is invalid: {}", plan.error().message));
        }

        list.isSmart = true;
        list.filter = *filter;
        return {};
      }

      if (tracksNode.readable())
      {
        return validateManualListTracks(tracksNode, list);
      }

      return {};
    }

    Result<ValidatedList> validateListRecord(ryml::ConstNodeRef const& listNode,
                                             std::unordered_set<std::uint32_t>& seenYamlIds)
    {
      if (auto result = requireMap(listNode, "List record"); !result)
      {
        return std::unexpected{result.error()};
      }

      if (auto result = rejectUnknownFields(listNode, kListFields, "List record"); !result)
      {
        return std::unexpected{result.error()};
      }

      auto list = ValidatedList{};

      if (auto result = validateListIdentity(listNode, seenYamlIds, list); !result)
      {
        return std::unexpected{result.error()};
      }

      if (auto result = validateListMetadata(listNode, list); !result)
      {
        return std::unexpected{result.error()};
      }

      if (auto result = validateListContents(listNode, list); !result)
      {
        return std::unexpected{result.error()};
      }

      return list;
    }
  } // namespace

  struct LibraryYamlImportOperation::PreparedImport::Impl final
  {
    Impl(std::filesystem::path sourcePathValue, ImportMode modeValue, bool buildChangeSetValue)
      : sourcePath{std::move(sourcePathValue)}
      , mode{modeValue}
      , buildChangeSet{buildChangeSetValue}
      , yamlErrorState{sourcePath.string()}
      , tree{yaml::callbacks(yamlErrorState)}
    {
    }

    std::filesystem::path sourcePath;
    ImportMode mode = ImportMode::Restore;
    bool buildChangeSet = false;
    std::vector<char> sourceBytes;
    std::vector<char> buffer;
    yaml::ErrorCallbackState yamlErrorState;
    ryml::Tree tree;
    ValidatedImport validated;
    std::vector<PreparedTrack> tracks;
    ImportReport initialReport;
    std::vector<TrackId> beforeTrackIds;
    std::vector<ListId> beforeListIds;
  };

  struct LibraryYamlImporter::Impl final
  {
    explicit Impl(library::MusicLibrary& ml)
      : ml{ml}
    {
    }

    Result<ImportReport> applyPreparedImport(LibraryYamlImportOperation::PreparedImport::Impl const& prepared,
                                             ImportRunMode runMode,
                                             library::WriteTransaction* externalTransaction = nullptr,
                                             LibraryChangeSet* preparedChangeSet = nullptr);
    Result<> applyPreparedRecords(LibraryYamlImportOperation::PreparedImport::Impl const& prepared,
                                  library::WriteTransaction& transaction,
                                  ImportReport& report) const;
    Result<> restoreLibraryIdentity(ValidatedImport const& validated,
                                    ImportMode mode,
                                    library::WriteTransaction& transaction) const;
    LibraryChangeSet buildPreparedChangeSet(LibraryYamlImportOperation::PreparedImport::Impl const& prepared,
                                            library::WriteTransaction const& transaction) const;
    Result<std::vector<PreparedTrack>> prepareTracks(ValidatedImport const& validated, ImportMode mode) const;

    void populateDeletionStats(ValidatedImport const& val, ImportReport& rep) const;
    Result<> clearDatabase(ValidatedImport const& val, library::WriteTransaction& writeTransaction) const;

    Result<ValidatedImport> validate(ryml::ConstNodeRef const& root) const;
    Result<> validateHeader(ryml::ConstNodeRef const& root, ValidatedImport& validated) const;
    Result<> validateLibrary(ryml::ConstNodeRef const& root, ValidatedImport& validated) const;
    Result<> validateTracks(ryml::ConstNodeRef const& tracks, ValidatedImport& validated) const;
    Result<> validateLists(ryml::ConstNodeRef const& lists, ValidatedImport& validated) const;

    Result<> importTracks(std::vector<PreparedTrack> const& tracks,
                          library::WriteTransaction& transaction,
                          std::unordered_map<std::uint32_t, TrackId>& yamlTrackIdToInternalId,
                          ImportMode strategy,
                          ImportReport& report) const;
    Result<> importTrackRecord(PreparedTrack const& preparedTrack,
                               library::WriteTransaction& transaction,
                               library::TrackStore::Writer& trackWriter,
                               library::FileManifestStore::Writer& manifestWriter,
                               library::FileManifestStore::Reader const& manifestReader,
                               ImportMode strategy,
                               std::unordered_map<std::uint32_t, TrackId>& yamlTrackIdToInternalId,
                               ImportReport& report) const;
    Result<> loadTrackBaseline(std::string_view uriStr,
                               std::optional<TrackId> const& optExistingTrackId,
                               ExportMode payloadMode,
                               std::optional<library::TrackBuilder>& optBuilder,
                               std::optional<MediaTrack>& optMediaTrack,
                               library::TrackStore::Reader const& trackReader) const;

    Result<> importLists(std::vector<ValidatedList> const& lists,
                         library::WriteTransaction& transaction,
                         std::unordered_map<std::uint32_t, TrackId> const& yamlTrackIdToInternalId,
                         ImportMode strategy,
                         ImportReport& report) const;

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

    Result<> buildStaticListTracks(library::ListBuilder& builder,
                                   ValidatedList const& importedList,
                                   std::unordered_map<std::uint32_t, TrackId> const& yamlTrackIdToInternalId,
                                   library::FileManifestStore::Reader const& manifestReader,
                                   ImportReport& report) const;
    Result<> updateListParent(ValidatedList const& importedList,
                              std::unordered_map<std::uint32_t, ListId> const& yamlListIdToNewListId,
                              library::ListStore::Writer& listWriter,
                              ImportReport& report) const;

    Result<> overlayMetadata(library::TrackBuilder& builder, ryml::ConstNodeRef const& trackNode) const;
    Result<> overlayTagsAndCustomMetadata(library::TrackBuilder& builder, ryml::ConstNodeRef const& trackNode) const;
    Result<> overlayTechnicalProperties(library::TrackBuilder& builder, ryml::ConstNodeRef const& trackNode) const;

    Result<> loadFileBaseline(std::string_view uriStr,
                              ExportMode payloadMode,
                              std::optional<library::TrackBuilder>& optBuilder,
                              std::optional<MediaTrack>& optMediaTrack) const;

    library::MusicLibrary& ml;
  };

  LibraryYamlImporter::LibraryYamlImporter(library::MusicLibrary& ml)
    : _implPtr{std::make_unique<Impl>(ml)}
  {
  }

  LibraryYamlImporter::~LibraryYamlImporter() = default;

  LibraryYamlImportOperation::PreparedImport::PreparedImport(std::unique_ptr<Impl> implPtr)
    : _implPtr{std::move(implPtr)}
  {
  }

  LibraryYamlImportOperation::PreparedImport::~PreparedImport() = default;
  LibraryYamlImportOperation::PreparedImport::PreparedImport(PreparedImport&&) noexcept = default;
  LibraryYamlImportOperation::PreparedImport& LibraryYamlImportOperation::PreparedImport::operator=(
    PreparedImport&&) noexcept = default;

  LibraryYamlImportOperation::LibraryYamlImportOperation(LibraryYamlImporter& importer) noexcept
    : _importer{importer}
  {
  }

  Result<ImportReport> LibraryYamlImporter::importFromYamlOffline(std::filesystem::path const& path, ImportMode mode)
  {
    auto operation = LibraryYamlImportOperation{*this};
    auto preparedResult = operation.prepare(path, mode, false);

    if (!preparedResult)
    {
      return std::unexpected{preparedResult.error()};
    }

    return operation.applyOffline(*preparedResult);
  }

  Result<ImportReport> LibraryYamlImporter::previewImportFromYamlOffline(std::filesystem::path const& path,
                                                                         ImportMode mode)
  {
    auto operation = LibraryYamlImportOperation{*this};
    auto preparedResult = operation.prepare(path, mode, false);

    if (!preparedResult)
    {
      return std::unexpected{preparedResult.error()};
    }

    return operation.previewOffline(*preparedResult);
  }

  Result<LibraryYamlImportOperation::PreparedImport>
  LibraryYamlImportOperation::prepare(std::filesystem::path const& path, ImportMode mode, bool buildChangeSet)
  {
    auto preparedPtr = std::make_unique<PreparedImport::Impl>(path, mode, buildChangeSet);
    auto bufferResult = yaml::readFileResult(path);

    if (!bufferResult)
    {
      return makeError(
        Error::Code::IoError, std::format("Failed to read '{}': {}", path.string(), bufferResult.error().message));
    }

    preparedPtr->buffer = std::move(*bufferResult);
    preparedPtr->sourceBytes = preparedPtr->buffer;

    try
    {
      yaml::parseInPlace(preparedPtr->tree, preparedPtr->buffer, preparedPtr->yamlErrorState);
      preparedPtr->tree.resolve();
    }
    catch (std::exception const& exception)
    {
      return makeError(
        Error::Code::FormatRejected, std::format("Failed to parse '{}': {}", path.string(), exception.what()));
    }

    auto validationResult = _importer._implPtr->validate(preparedPtr->tree.rootref());

    if (!validationResult)
    {
      return std::unexpected{validationResult.error()};
    }

    preparedPtr->validated = std::move(*validationResult);
    preparedPtr->initialReport.payloadVersion = preparedPtr->validated.version;
    preparedPtr->initialReport.payloadMode = preparedPtr->validated.payloadMode;
    preparedPtr->initialReport.targetScope = preparedPtr->validated.payloadMode == ExportMode::ListOnly
                                               ? ImportTargetScope::Lists
                                               : ImportTargetScope::Library;

    if (mode == ImportMode::Restore)
    {
      _importer._implPtr->populateDeletionStats(preparedPtr->validated, preparedPtr->initialReport);
    }

    if (buildChangeSet && mode == ImportMode::Merge)
    {
      auto readTransaction = _importer._implPtr->ml.readTransaction();

      for (auto const [trackId, view] : _importer._implPtr->ml.tracks().reader(readTransaction).hot())
      {
        std::ignore = view;
        preparedPtr->beforeTrackIds.push_back(trackId);
      }

      for (auto const [listId, view] : _importer._implPtr->ml.lists().reader(readTransaction))
      {
        std::ignore = view;
        preparedPtr->beforeListIds.push_back(listId);
      }
    }

    auto tracksResult = _importer._implPtr->prepareTracks(preparedPtr->validated, mode);

    if (!tracksResult)
    {
      return std::unexpected{tracksResult.error()};
    }

    preparedPtr->tracks = std::move(*tracksResult);
    return PreparedImport{std::move(preparedPtr)};
  }

  Result<> LibraryYamlImportOperation::revalidateSource(PreparedImport const& prepared) const
  {
    auto currentBytes = yaml::readFileResult(prepared._implPtr->sourcePath);

    if (!currentBytes)
    {
      return makeError(
        Error::Code::IoError,
        std::format("Failed to reread '{}': {}", prepared._implPtr->sourcePath.string(), currentBytes.error().message));
    }

    if (*currentBytes != prepared._implPtr->sourceBytes)
    {
      return makeError(Error::Code::Conflict, "Import file changed after its preview was prepared");
    }

    return {};
  }

  Result<ImportReport> LibraryYamlImportOperation::apply(PreparedImport const& prepared,
                                                         library::WriteTransaction& transaction,
                                                         LibraryChangeSet& changeSet)
  {
    return _importer._implPtr->applyPreparedImport(*prepared._implPtr, ImportRunMode::Commit, &transaction, &changeSet);
  }

  Result<ImportReport> LibraryYamlImportOperation::preview(PreparedImport const& prepared,
                                                           library::WriteTransaction& transaction)
  {
    return _importer._implPtr->applyPreparedImport(*prepared._implPtr, ImportRunMode::Preview, &transaction);
  }

  Result<ImportReport> LibraryYamlImportOperation::applyOffline(PreparedImport const& prepared)
  {
    return _importer._implPtr->applyPreparedImport(*prepared._implPtr, ImportRunMode::Commit);
  }

  Result<ImportReport> LibraryYamlImportOperation::previewOffline(PreparedImport const& prepared)
  {
    return _importer._implPtr->applyPreparedImport(*prepared._implPtr, ImportRunMode::Preview);
  }

  Result<> LibraryYamlImporter::Impl::applyPreparedRecords(
    LibraryYamlImportOperation::PreparedImport::Impl const& prepared,
    library::WriteTransaction& transaction,
    ImportReport& report) const
  {
    auto const& validated = prepared.validated;

    if (prepared.mode == ImportMode::Restore)
    {
      if (auto const clearResult = clearDatabase(validated, transaction); !clearResult)
      {
        return clearResult;
      }
    }

    auto yamlTrackIdToInternalId = std::unordered_map<std::uint32_t, TrackId>{};

    if (!prepared.tracks.empty())
    {
      if (auto result = importTracks(prepared.tracks, transaction, yamlTrackIdToInternalId, prepared.mode, report);
          !result)
      {
        return result;
      }
    }

    if (!validated.lists.empty())
    {
      return importLists(validated.lists, transaction, yamlTrackIdToInternalId, prepared.mode, report);
    }

    return {};
  }

  Result<> LibraryYamlImporter::Impl::restoreLibraryIdentity(ValidatedImport const& validated,
                                                             ImportMode mode,
                                                             library::WriteTransaction& transaction) const
  {
    if (mode != ImportMode::Restore || validated.payloadMode == ExportMode::ListOnly || !validated.optLibraryId)
    {
      return {};
    }

    auto header = ml.metadata().load(transaction);

    if (!header)
    {
      return std::unexpected{header.error()};
    }

    header->libraryId = parseUuid(*validated.optLibraryId);
    return ml.metadata().update(transaction, *header);
  }

  LibraryChangeSet LibraryYamlImporter::Impl::buildPreparedChangeSet(
    LibraryYamlImportOperation::PreparedImport::Impl const& prepared,
    library::WriteTransaction const& transaction) const
  {
    auto changeSet = LibraryChangeSet{.libraryReset = prepared.mode == ImportMode::Restore};

    if (!prepared.buildChangeSet || prepared.mode != ImportMode::Merge)
    {
      return changeSet;
    }

    auto const beforeTracks = boost::unordered_flat_set<TrackId, std::hash<TrackId>>{
      prepared.beforeTrackIds.begin(), prepared.beforeTrackIds.end()};
    auto const beforeLists = boost::unordered_flat_set<ListId, std::hash<ListId>>{
      prepared.beforeListIds.begin(), prepared.beforeListIds.end()};

    for (auto const [trackId, view] : ml.tracks().reader(transaction).hot())
    {
      std::ignore = view;

      if (!beforeTracks.contains(trackId))
      {
        changeSet.tracksInserted.push_back(trackId);
      }
    }

    auto const manifestReader = ml.manifest().reader(transaction);

    for (auto const& importedTrack : prepared.tracks)
    {
      auto const manifestResult = manifestReader.get(importedTrack.uri);

      if (manifestResult && beforeTracks.contains(manifestResult->trackId()))
      {
        changeSet.tracksMutated.push_back(manifestResult->trackId());
      }
    }

    for (auto const [listId, view] : ml.lists().reader(transaction))
    {
      std::ignore = view;

      if (!beforeLists.contains(listId))
      {
        changeSet.listsUpserted.push_back(listId);
      }
    }

    return changeSet;
  }

  Result<ImportReport> LibraryYamlImporter::Impl::applyPreparedImport(
    LibraryYamlImportOperation::PreparedImport::Impl const& prepared,
    ImportRunMode runMode,
    library::WriteTransaction* externalTransaction,
    LibraryChangeSet* preparedChangeSet)
  {
    auto report = prepared.initialReport;
    auto optOwnedTransaction = std::optional<library::WriteTransaction>{};

    if (externalTransaction == nullptr)
    {
      auto writableResult = library::WritableMusicLibrary::acquire(ml);

      if (!writableResult)
      {
        return std::unexpected{writableResult.error()};
      }

      optOwnedTransaction.emplace(writableResult->writeTransaction());
      externalTransaction = &*optOwnedTransaction;
    }

    auto& transaction = *externalTransaction;
    auto applyResult = applyPreparedRecords(prepared, transaction, report);

    if (!applyResult)
    {
      return std::unexpected{applyResult.error()};
    }

    if (runMode == ImportRunMode::Preview)
    {
      return report;
    }

    if (auto identityResult = restoreLibraryIdentity(prepared.validated, prepared.mode, transaction); !identityResult)
    {
      return std::unexpected{identityResult.error()};
    }

    auto changeSet = buildPreparedChangeSet(prepared, transaction);

    if (preparedChangeSet != nullptr)
    {
      *preparedChangeSet = std::move(changeSet);
      return report;
    }

    if (auto result = transaction.commit(); !result)
    {
      return std::unexpected{result.error()};
    }

    return report;
  }

  void LibraryYamlImporter::Impl::populateDeletionStats(ValidatedImport const& val, ImportReport& rep) const
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
                                                    library::WriteTransaction& writeTransaction) const
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

  Result<ValidatedImport> LibraryYamlImporter::Impl::validate(ryml::ConstNodeRef const& root) const
  {
    if (auto result = requireMap(root, "YAML root"); !result)
    {
      return std::unexpected{result.error()};
    }

    auto validated = ValidatedImport{};

    if (auto result = validateHeader(root, validated); !result)
    {
      return std::unexpected{result.error()};
    }

    if (auto result = validateLibrary(root, validated); !result)
    {
      return std::unexpected{result.error()};
    }

    return validated;
  }

  Result<> LibraryYamlImporter::Impl::validateHeader(ryml::ConstNodeRef const& root, ValidatedImport& validated) const
  {
    auto const versionNode = yaml::findChild(root, "version");

    if (!versionNode.readable())
    {
      return makeError(Error::Code::FormatRejected, "Missing 'version' field in YAML");
    }

    auto version = requireScalarAs<std::uint32_t>(versionNode, "version");

    if (!version)
    {
      return std::unexpected{version.error()};
    }

    constexpr std::uint32_t kYamlFormatVersion = 2;

    if (*version != kYamlFormatVersion)
    {
      return makeError(Error::Code::FormatRejected, std::format("Unsupported YAML version {}", *version));
    }

    validated.version = *version;

    if (auto result = rejectUnknownFields(root, kRootFields, "YAML root"); !result)
    {
      return result;
    }

    auto exportModeText = requireScalarField(root, "export_mode", "YAML root");

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

    return {};
  }

  Result<> LibraryYamlImporter::Impl::validateLibrary(ryml::ConstNodeRef const& root, ValidatedImport& validated) const
  {
    auto const library = yaml::findChild(root, "library");

    if (!library.readable())
    {
      return makeError(Error::Code::FormatRejected, "Missing 'library' section in YAML");
    }

    if (auto result = requireMap(library, "library"); !result)
    {
      return std::unexpected{result.error()};
    }

    if (auto result = rejectUnknownFields(library, kLibraryFields, "library"); !result)
    {
      return result;
    }

    auto const tracks = yaml::findChild(library, "tracks");
    auto const lists = yaml::findChild(library, "lists");

    if (validated.payloadMode == ExportMode::ListOnly)
    {
      if (tracks.readable())
      {
        return makeError(Error::Code::FormatRejected, "library.tracks is forbidden for a listOnly payload");
      }
    }
    else
    {
      if (!tracks.readable())
      {
        return makeError(Error::Code::FormatRejected, "library missing required 'tracks' field");
      }

      if (auto result = requireSequence(tracks, "library.tracks"); !result)
      {
        return std::unexpected{result.error()};
      }

      if (auto result = validateTracks(tracks, validated); !result)
      {
        return std::unexpected{result.error()};
      }
    }

    if (!lists.readable())
    {
      return makeError(Error::Code::FormatRejected, "library missing required 'lists' field");
    }

    if (auto result = requireSequence(lists, "library.lists"); !result)
    {
      return std::unexpected{result.error()};
    }

    if (auto result = validateLists(lists, validated); !result)
    {
      return std::unexpected{result.error()};
    }

    return {};
  }

  Result<> LibraryYamlImporter::Impl::validateTracks(ryml::ConstNodeRef const& tracks, ValidatedImport& validated) const
  {
    auto seenYamlIds = std::unordered_set<std::uint32_t>{};
    auto seenUris = std::unordered_set<std::string>{};

    for (auto const& trackNode : tracks.children())
    {
      if (auto result = requireMap(trackNode, "Track record"); !result)
      {
        return std::unexpected{result.error()};
      }

      if (auto result = rejectUnknownFields(trackNode, kTrackFields, "Track record"); !result)
      {
        return std::unexpected{result.error()};
      }

      if (auto result = validateTrackNestedSchema(trackNode); !result)
      {
        return std::unexpected{result.error()};
      }

      auto track = ValidatedTrack{};
      auto uri = requireScalarField(trackNode, "uri", "Track record");

      if (!uri)
      {
        return std::unexpected{uri.error()};
      }

      auto parsedUri = parseLibraryUri(*uri, "Track record.uri");

      if (!parsedUri)
      {
        return std::unexpected{parsedUri.error()};
      }

      track.uri = std::move(*parsedUri);

      if (!seenUris.insert(track.uri).second)
      {
        return makeError(
          Error::Code::FormatRejected, std::format("Duplicate canonical track URI '{}' in payload", track.uri));
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

  Result<> LibraryYamlImporter::Impl::validateLists(ryml::ConstNodeRef const& lists, ValidatedImport& validated) const
  {
    auto seenYamlIds = std::unordered_set<std::uint32_t>{};

    for (auto const& listNode : lists.children())
    {
      auto list = validateListRecord(listNode, seenYamlIds);

      if (!list)
      {
        return std::unexpected{list.error()};
      }

      validated.lists.push_back(std::move(*list));
    }

    auto parents = std::unordered_map<std::uint32_t, std::uint32_t>{};
    parents.reserve(validated.lists.size());

    for (auto const& list : validated.lists)
    {
      parents.emplace(list.yamlId, list.yamlParentId);
    }

    auto resolved = std::unordered_set<std::uint32_t>{};
    resolved.reserve(parents.size());

    for (auto const& list : validated.lists)
    {
      if (resolved.contains(list.yamlId))
      {
        continue;
      }

      auto seenAncestors = std::unordered_set<std::uint32_t>{};
      auto path = std::vector<std::uint32_t>{};
      auto cursor = list.yamlId;

      while (cursor != 0 && !resolved.contains(cursor))
      {
        if (!seenAncestors.insert(cursor).second)
        {
          return makeError(
            Error::Code::FormatRejected, std::format("List parent graph contains a cycle at YAML id {}", cursor));
        }

        path.push_back(cursor);

        auto const parent = parents.find(cursor);

        if (parent == parents.end())
        {
          break;
        }

        cursor = parent->second;
      }

      resolved.insert(path.begin(), path.end());
    }

    return {};
  }

  Result<std::vector<PreparedTrack>> LibraryYamlImporter::Impl::prepareTracks(ValidatedImport const& validated,
                                                                              ImportMode mode) const
  {
    auto result = std::vector<PreparedTrack>{};
    result.reserve(validated.tracks.size());
    auto transaction = ml.readTransaction();
    auto trackReader = ml.tracks().reader(transaction);
    auto manifestReader = ml.manifest().reader(transaction);

    for (auto const& validatedTrack : validated.tracks)
    {
      auto optExistingTrackId = std::optional<TrackId>{};

      if (mode == ImportMode::Merge)
      {
        if (auto const manifestResult = manifestReader.get(validatedTrack.uri); manifestResult)
        {
          optExistingTrackId = manifestResult->trackId();
        }
      }

      auto optMediaTrack = std::optional<MediaTrack>{};
      auto optBuilder = std::optional<library::TrackBuilder>{};

      if (auto baselineResult = loadTrackBaseline(
            validatedTrack.uri, optExistingTrackId, validated.payloadMode, optBuilder, optMediaTrack, trackReader);
          !baselineResult)
      {
        return std::unexpected{baselineResult.error()};
      }

      auto builder = optBuilder ? *optBuilder : library::TrackBuilder::makeEmpty();

      if (!optBuilder)
      {
        builder.property().uri(validatedTrack.uri);
      }

      if (auto overlayResult = overlayMetadata(builder, validatedTrack.node); !overlayResult)
      {
        return std::unexpected{overlayResult.error()};
      }

      if (auto overlayResult = overlayTagsAndCustomMetadata(builder, validatedTrack.node); !overlayResult)
      {
        return std::unexpected{overlayResult.error()};
      }

      if (auto overlayResult = overlayTechnicalProperties(builder, validatedTrack.node); !overlayResult)
      {
        return std::unexpected{overlayResult.error()};
      }

      auto decodedCoverBlobsResult = importCovers(validatedTrack.node, builder);

      if (!decodedCoverBlobsResult)
      {
        return std::unexpected{decodedCoverBlobsResult.error()};
      }

      auto decodedCoverBlobs = std::move(*decodedCoverBlobsResult);
      auto manifestBuilder = library::FileManifestBuilder::makeEmpty();

      if (auto metadataResult =
            applyFileMetadata(validatedTrack.node, validatedTrack.uri, manifestReader, manifestBuilder);
          !metadataResult)
      {
        return std::unexpected{metadataResult.error()};
      }

      result.push_back(PreparedTrack{.yamlId = validatedTrack.yamlId,
                                     .uri = validatedTrack.uri,
                                     .optExistingTrackId = optExistingTrackId,
                                     .builder = TrackBuilderSnapshot{builder},
                                     .manifest = std::move(manifestBuilder)});
    }

    return result;
  }

  Result<> LibraryYamlImporter::Impl::importTracks(std::vector<PreparedTrack> const& tracks,
                                                   library::WriteTransaction& transaction,
                                                   std::unordered_map<std::uint32_t, TrackId>& yamlTrackIdToInternalId,
                                                   ImportMode strategy,
                                                   ImportReport& report) const
  {
    auto trackWriter = ml.tracks().writer(transaction);
    auto manifestWriter = ml.manifest().writer(transaction);
    auto manifestReader = ml.manifest().reader(transaction);

    for (auto const& preparedTrack : tracks)
    {
      if (auto result = importTrackRecord(preparedTrack,
                                          transaction,
                                          trackWriter,
                                          manifestWriter,
                                          manifestReader,
                                          strategy,
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
    PreparedTrack const& preparedTrack,
    library::WriteTransaction& transaction,
    library::TrackStore::Writer& trackWriter,
    library::FileManifestStore::Writer& manifestWriter,
    library::FileManifestStore::Reader const& manifestReader,
    ImportMode strategy,
    std::unordered_map<std::uint32_t, TrackId>& yamlTrackIdToInternalId,
    ImportReport& report) const
  {
    auto const& resources = ml.resources();
    auto const& uri = preparedTrack.uri;
    auto optExistingTrackId = preparedTrack.optExistingTrackId;

    if (strategy == ImportMode::Merge)
    {
      auto optCurrentTrackId = std::optional<TrackId>{};

      if (auto const manifestResult = manifestReader.get(uri); manifestResult)
      {
        optCurrentTrackId = manifestResult->trackId();
      }

      if (optCurrentTrackId != optExistingTrackId)
      {
        return makeError(
          Error::Code::InvalidState, std::format("Library changed after preparing YAML track '{}'", uri));
      }
    }

    auto builder = preparedTrack.builder.makeBuilder();
    auto preparedResult = builder.prepare(transaction, resources);

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

    auto manifestBuilder = preparedTrack.manifest;
    manifestBuilder.trackId(targetTrackId);

    if (auto putResult = manifestWriter.put(uri, manifestBuilder.serialize()); !putResult)
    {
      return std::unexpected{putResult.error()};
    }

    if (preparedTrack.yamlId != 0)
    {
      yamlTrackIdToInternalId[preparedTrack.yamlId] = targetTrackId;
    }

    return {};
  }

  Result<std::vector<std::vector<std::byte>>> LibraryYamlImporter::Impl::importCovers(
    ryml::ConstNodeRef const& trackNode,
    library::TrackBuilder& builder) const
  {
    auto decodedCoverBlobs = std::vector<std::vector<std::byte>>{};

    if (auto const coversNode = yaml::findChild(trackNode, "covers"); coversNode.readable())
    {
      if (!coversNode.is_seq())
      {
        return makeError(Error::Code::FormatRejected, "Track covers must be a sequence");
      }

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

        if (*rawType > static_cast<std::uint32_t>(PictureType::PublisherLogo))
        {
          return makeError(Error::Code::FormatRejected, std::format("Unknown cover type {}", *rawType));
        }

        auto const picType = static_cast<PictureType>(*rawType);

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
      auto uri = library::LibraryUri::parse(uriStr);

      if (!uri)
      {
        return makeError(Error::Code::FormatRejected, uri.error().message);
      }

      auto fullPathResult = uri->resolveUnder(ml.rootPath());

      if (!fullPathResult)
      {
        auto const code = fullPathResult.error().code == Error::Code::InvalidInput ? Error::Code::FormatRejected
                                                                                   : fullPathResult.error().code;
        return makeError(code, fullPathResult.error().message);
      }

      if (auto const& fullPath = *fullPathResult; std::filesystem::exists(fullPath, fileEc) && !fileEc)
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
                                                        std::optional<MediaTrack>& optMediaTrack,
                                                        library::TrackStore::Reader const& trackReader) const
  {
    if (optExistingTrackId)
    {
      if (auto optView = trackReader.get(*optExistingTrackId, library::TrackStore::Reader::LoadMode::Both); optView)
      {
        optBuilder = library::TrackBuilder::fromView(*optView, ml.dictionary());
      }
    }

    if (payloadMode == ExportMode::Delta || payloadMode == ExportMode::Metadata)
    {
      if (auto result = loadFileBaseline(uriStr, payloadMode, optBuilder, optMediaTrack); !result)
      {
        return std::unexpected{result.error()};
      }
    }

    return {};
  }

  Result<> LibraryYamlImporter::Impl::loadFileBaseline(std::string_view uriStr,
                                                       ExportMode payloadMode,
                                                       std::optional<library::TrackBuilder>& optBuilder,
                                                       std::optional<MediaTrack>& optMediaTrack) const
  {
    auto fileEc = std::error_code{};
    auto uri = library::LibraryUri::parse(uriStr);

    if (!uri)
    {
      return makeError(Error::Code::FormatRejected, uri.error().message);
    }

    auto fullPathResult = uri->resolveUnder(ml.rootPath());

    if (!fullPathResult)
    {
      auto const code = fullPathResult.error().code == Error::Code::InvalidInput ? Error::Code::FormatRejected
                                                                                 : fullPathResult.error().code;
      return makeError(code, fullPathResult.error().message);
    }

    auto const& fullPath = *fullPathResult;
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

    auto mediaTrackResult = readMediaTrack(fullPath);

    if (!mediaTrackResult)
    {
      return {};
    }

    optMediaTrack.emplace(std::move(*mediaTrackResult));

    if (!optBuilder)
    {
      optBuilder = optMediaTrack->builder();

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

    auto const& fileBuilder = optMediaTrack->builder();
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
      else
      {
        return makeError(Error::Code::FormatRejected, std::format("Unknown codec '{}'", *codec));
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
    library::WriteTransaction& transaction,
    std::unordered_map<std::uint32_t, TrackId> const& yamlTrackIdToInternalId,
    ImportMode /*strategy*/,
    ImportReport& report) const
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
        if (auto result = buildStaticListTracks(builder, importedList, yamlTrackIdToInternalId, manifestReader, report);
            !result)
        {
          return result;
        }
      }

      auto payload = builder.serialize();

      if (!payload)
      {
        return makeError(
          Error::Code::FormatRejected,
          std::format(
            "List YAML id {} exceeds the binary storage limits: {}", importedList.yamlId, payload.error().message));
      }

      auto createResult = listWriter.create(*payload);

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
      if (auto result = updateListParent(importedList, yamlListIdToNewListId, listWriter, report); !result)
      {
        return std::unexpected{result.error()};
      }
    }

    return {};
  }

  Result<> LibraryYamlImporter::Impl::buildStaticListTracks(
    library::ListBuilder& builder,
    ValidatedList const& importedList,
    std::unordered_map<std::uint32_t, TrackId> const& yamlTrackIdToInternalId,
    library::FileManifestStore::Reader const& manifestReader,
    ImportReport& report) const
  {
    for (auto const& trackReference : importedList.trackReferences)
    {
      if (auto const* idReference = std::get_if<ValidatedListTrackIdReference>(&trackReference); idReference != nullptr)
      {
        if (auto const it = yamlTrackIdToInternalId.find(idReference->yamlId); it != yamlTrackIdToInternalId.end())
        {
          builder.tracks().add(it->second);
        }
        else
        {
          ++report.danglingReferencesIgnored;
        }

        continue;
      }

      auto const& uriReference = std::get<ValidatedListTrackUriReference>(trackReference);

      if (auto const manifestResult = manifestReader.get(uriReference.uri); manifestResult)
      {
        builder.tracks().add(manifestResult->trackId());
      }
      else if (manifestResult.error().code == Error::Code::NotFound)
      {
        ++report.danglingReferencesIgnored;
      }
      else
      {
        return std::unexpected{manifestResult.error()};
      }
    }

    return {};
  }

  Result<> LibraryYamlImporter::Impl::updateListParent(
    ValidatedList const& importedList,
    std::unordered_map<std::uint32_t, ListId> const& yamlListIdToNewListId,
    library::ListStore::Writer& listWriter,
    ImportReport& report) const
  {
    if (importedList.yamlParentId == 0)
    {
      return {};
    }

    auto const parentIt = yamlListIdToNewListId.find(importedList.yamlParentId);

    if (parentIt == yamlListIdToNewListId.end())
    {
      ++report.danglingReferencesIgnored;
      return {};
    }

    auto const childId = yamlListIdToNewListId.at(importedList.yamlId);
    auto optListView = listWriter.get(childId);

    if (!optListView)
    {
      return {};
    }

    auto builder = library::ListBuilder::fromView(*optListView).parentId(parentIt->second);
    auto payload = builder.serialize();

    if (!payload)
    {
      return std::unexpected{payload.error()};
    }

    if (auto result = listWriter.update(childId, *payload); !result)
    {
      return std::unexpected{result.error()};
    }

    return {};
  }
} // namespace ao::rt
