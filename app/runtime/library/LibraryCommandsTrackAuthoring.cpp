// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "runtime/library/LibraryCommandsInternal.h"
#include "runtime/library/LibraryWriteLane.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Task.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryCommands.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ao::rt
{
  namespace
  {
    struct PatchResult final
    {
      bool changedHot = false;
      bool changedCold = false;
    };

    Result<MetadataPatch> normalizeMetadataPatch(MetadataPatch const& patch)
    {
      auto normalized = patch;
      constexpr auto kTextMembers = std::to_array<std::optional<std::string> MetadataPatch::*>({
        &MetadataPatch::optTitle,
        &MetadataPatch::optArtist,
        &MetadataPatch::optAlbum,
        &MetadataPatch::optAlbumArtist,
        &MetadataPatch::optGenre,
        &MetadataPatch::optComposer,
        &MetadataPatch::optConductor,
        &MetadataPatch::optEnsemble,
        &MetadataPatch::optWork,
        &MetadataPatch::optMovement,
        &MetadataPatch::optSoloist,
      });

      for (auto const member : kTextMembers)
      {
        auto& optValue = normalized.*member;

        if (!optValue)
        {
          continue;
        }

        auto valueRes = detail::normalizeRuntimeText(*optValue, "Track metadata");

        if (!valueRes)
        {
          return std::unexpected{valueRes.error()};
        }

        optValue = std::move(*valueRes);
      }

      auto customUpdates = std::move(normalized.customUpdates);
      normalized.customUpdates.clear();

      for (auto const& [key, optValue] : customUpdates)
      {
        if (key.empty())
        {
          continue;
        }

        auto keyRes = detail::normalizeRuntimeText(key, "Custom metadata key");

        if (!keyRes)
        {
          return std::unexpected{keyRes.error()};
        }

        auto optNormalizedValue = std::optional<std::string>{};

        if (optValue)
        {
          auto valueRes = detail::normalizeRuntimeText(*optValue, "Custom metadata value");

          if (!valueRes)
          {
            return std::unexpected{valueRes.error()};
          }

          optNormalizedValue = std::move(*valueRes);
        }

        if (!normalized.customUpdates.emplace(std::move(*keyRes), std::move(optNormalizedValue)).second)
        {
          return makeError(Error::Code::InvalidInput, "Custom metadata keys must be unique after NFC normalization");
        }
      }

      return normalized;
    }

    template<typename Setter>
    void applyStringPatch(std::optional<std::string> const& optValue,
                          std::string_view fieldName,
                          std::string_view current,
                          Setter setter,
                          bool& changed,
                          std::vector<TrackFieldChange>& changes)
    {
      if (!optValue || current == *optValue)
      {
        return;
      }

      changes.push_back(
        TrackFieldChange{.field = std::string{fieldName}, .oldValue = std::string{current}, .newValue = *optValue});
      setter(*optValue);
      changed = true;
    }

    template<typename Setter>
    void applyUint16Patch(std::optional<std::uint16_t> const& optValue,
                          std::string_view fieldName,
                          std::uint16_t current,
                          Setter setter,
                          bool& changed,
                          std::vector<TrackFieldChange>& changes)
    {
      if (!optValue || current == *optValue)
      {
        return;
      }

      changes.push_back(TrackFieldChange{.field = std::string{fieldName},
                                         .oldValue = std::format("{}", current),
                                         .newValue = std::format("{}", *optValue)});
      setter(*optValue);
      changed = true;
    }

    void applyTextMetadataPatch(library::TrackBuilder::MetadataBuilder& metadata,
                                MetadataPatch const& patch,
                                PatchResult& result,
                                std::vector<TrackFieldChange>& changes)
    {
      applyStringPatch(
        patch.optTitle,
        "title",
        metadata.title(),
        [&metadata](std::string_view value) { metadata.title(value); },
        result.changedHot,
        changes);
      applyStringPatch(
        patch.optArtist,
        "artist",
        metadata.artist(),
        [&metadata](std::string_view value) { metadata.artist(value); },
        result.changedHot,
        changes);
      applyStringPatch(
        patch.optAlbum,
        "album",
        metadata.album(),
        [&metadata](std::string_view value) { metadata.album(value); },
        result.changedHot,
        changes);
      applyStringPatch(
        patch.optAlbumArtist,
        "albumArtist",
        metadata.albumArtist(),
        [&metadata](std::string_view value) { metadata.albumArtist(value); },
        result.changedHot,
        changes);
      applyStringPatch(
        patch.optGenre,
        "genre",
        metadata.genre(),
        [&metadata](std::string_view value) { metadata.genre(value); },
        result.changedHot,
        changes);
      applyStringPatch(
        patch.optComposer,
        "composer",
        metadata.composer(),
        [&metadata](std::string_view value) { metadata.composer(value); },
        result.changedHot,
        changes);
      applyStringPatch(
        patch.optConductor,
        "conductor",
        metadata.conductor(),
        [&metadata](std::string_view value) { metadata.conductor(value); },
        result.changedCold,
        changes);
      applyStringPatch(
        patch.optEnsemble,
        "ensemble",
        metadata.ensemble(),
        [&metadata](std::string_view value) { metadata.ensemble(value); },
        result.changedCold,
        changes);
      applyStringPatch(
        patch.optWork,
        "work",
        metadata.work(),
        [&metadata](std::string_view value) { metadata.work(value); },
        result.changedCold,
        changes);
      applyStringPatch(
        patch.optMovement,
        "movement",
        metadata.movement(),
        [&metadata](std::string_view value) { metadata.movement(value); },
        result.changedCold,
        changes);
      applyStringPatch(
        patch.optSoloist,
        "soloist",
        metadata.soloist(),
        [&metadata](std::string_view value) { metadata.soloist(value); },
        result.changedCold,
        changes);
    }

    void applyNumberMetadataPatch(library::TrackBuilder::MetadataBuilder& metadata,
                                  MetadataPatch const& patch,
                                  PatchResult& result,
                                  std::vector<TrackFieldChange>& changes)
    {
      applyUint16Patch(
        patch.optYear,
        "year",
        metadata.year(),
        [&metadata](std::uint16_t value) { metadata.year(value); },
        result.changedHot,
        changes);
      applyUint16Patch(
        patch.optMovementNumber,
        "movementNumber",
        metadata.movementNumber(),
        [&metadata](std::uint16_t value) { metadata.movementNumber(value); },
        result.changedCold,
        changes);
      applyUint16Patch(
        patch.optMovementTotal,
        "movementTotal",
        metadata.movementTotal(),
        [&metadata](std::uint16_t value) { metadata.movementTotal(value); },
        result.changedCold,
        changes);
      applyUint16Patch(
        patch.optTrackNumber,
        "trackNumber",
        metadata.trackNumber(),
        [&metadata](std::uint16_t value) { metadata.trackNumber(value); },
        result.changedCold,
        changes);
      applyUint16Patch(
        patch.optTrackTotal,
        "trackTotal",
        metadata.trackTotal(),
        [&metadata](std::uint16_t value) { metadata.trackTotal(value); },
        result.changedCold,
        changes);
      applyUint16Patch(
        patch.optDiscNumber,
        "discNumber",
        metadata.discNumber(),
        [&metadata](std::uint16_t value) { metadata.discNumber(value); },
        result.changedCold,
        changes);
      applyUint16Patch(
        patch.optDiscTotal,
        "discTotal",
        metadata.discTotal(),
        [&metadata](std::uint16_t value) { metadata.discTotal(value); },
        result.changedCold,
        changes);
    }

    void applyCustomMetadataPatch(library::TrackBuilder& builder,
                                  MetadataPatch const& patch,
                                  PatchResult& result,
                                  std::vector<TrackFieldChange>& changes)
    {
      for (auto const& [key, optValue] : patch.customUpdates)
      {
        if (key.empty())
        {
          continue;
        }

        auto const& pairs = builder.customMetadata().pairs();
        auto const existing =
          std::ranges::find_if(pairs, [&key](auto const& pair) { return pair.first == std::string_view{key}; });

        if (existing == pairs.end() && !optValue)
        {
          continue;
        }

        if (existing != pairs.end() && optValue && existing->second == *optValue)
        {
          continue;
        }

        changes.push_back(
          TrackFieldChange{.field = "custom." + key,
                           .oldValue = existing != pairs.end() ? std::string{existing->second} : std::string{},
                           .newValue = optValue ? *optValue : std::string{}});
        builder.customMetadata().remove(key);

        if (optValue)
        {
          builder.customMetadata().add(key, *optValue);
        }

        result.changedCold = true;
      }
    }

    PatchResult applyMetadataPatch(library::TrackBuilder& builder,
                                   MetadataPatch const& patch,
                                   std::vector<TrackFieldChange>& changes)
    {
      auto& metadata = builder.metadata();
      auto result = PatchResult{};

      applyTextMetadataPatch(metadata, patch, result, changes);
      applyNumberMetadataPatch(metadata, patch, result, changes);
      applyCustomMetadataPatch(builder, patch, result, changes);

      return result;
    }

    Result<UpdateTrackMetadataReply> applyMetadataPatchInTransaction(library::MusicLibrary& library,
                                                                     library::LibraryWrite& transaction,
                                                                     std::span<TrackId const> trackIds,
                                                                     MetadataPatch const& patch)
    {
      auto normalizedPatchRes = normalizeMetadataPatch(patch);

      if (!normalizedPatchRes)
      {
        return std::unexpected{normalizedPatchRes.error()};
      }

      auto const& normalizedPatch = *normalizedPatchRes;
      auto writer = transaction.tracks();
      auto changes = std::vector<TrackChangeRecord>{};

      for (auto const trackId : trackIds)
      {
        auto optView = writer.get(trackId, library::TrackStore::Reader::LoadMode::Both);

        if (!optView)
        {
          continue;
        }

        auto builder = library::TrackBuilder::fromCompleteView(*optView, library.dictionary());
        auto fieldChanges = std::vector<TrackFieldChange>{};
        auto const patchRes = applyMetadataPatch(builder, normalizedPatch, fieldChanges);

        if (!patchRes.changedHot && !patchRes.changedCold)
        {
          continue;
        }

        auto updateRes = Result<>{};

        if (patchRes.changedHot && patchRes.changedCold)
        {
          updateRes = writer.update(trackId, builder);
        }
        else if (patchRes.changedHot)
        {
          updateRes = writer.updateHot(trackId, builder);
        }
        else
        {
          updateRes = writer.updateCold(trackId, builder);
        }

        if (!updateRes)
        {
          return detail::storageError("Failed to update track data", updateRes.error());
        }

        changes.push_back(TrackChangeRecord{.trackId = trackId, .fields = std::move(fieldChanges)});
      }

      return UpdateTrackMetadataReply{.changes = std::move(changes)};
    }

    Result<UpdateTrackPropertiesReply> applyPropertiesPatchInTransaction(library::MusicLibrary& library,
                                                                         library::LibraryWrite& transaction,
                                                                         std::span<TrackId const> trackIds,
                                                                         TrackPropertiesPatch const& patch)
    {
      auto metadataRes = applyMetadataPatchInTransaction(library, transaction, trackIds, patch.metadata);

      if (!metadataRes)
      {
        return std::unexpected{metadataRes.error()};
      }

      auto tagsRes =
        detail::applyTagPatchInTransaction(library, transaction, trackIds, patch.tagsToAdd, patch.tagsToRemove);

      if (!tagsRes)
      {
        return std::unexpected{tagsRes.error()};
      }

      return UpdateTrackPropertiesReply{.metadata = std::move(*metadataRes), .tags = std::move(*tagsRes)};
    }
  } // namespace

  async::Task<Result<UpdateTrackMetadataReply>> LibraryCommands::Impl::previewUpdateMetadata(
    LibraryWriteLane::Submission submission,
    std::vector<TrackId> trackIds,
    MetadataPatch patch)
  {
    return detail::applyInteractivePreviewAsync(
      std::move(submission),
      [this, trackIds = std::move(trackIds), patch = std::move(patch)](library::LibraryWrite& transaction)
      { return applyMetadataPatchInTransaction(library, transaction, trackIds, patch); });
  }

  async::Task<Result<TrackAuthoringResult<UpdateTrackMetadataReply>>> LibraryCommands::Impl::applyUpdateMetadata(
    LibraryWriteLane::Submission submission,
    BoundTrackTargets targets,
    MetadataPatch patch)
  {
    return detail::executeBoundTrackAuthoringAsync<UpdateTrackMetadataReply>(
      std::move(submission),
      std::move(targets),
      "Update track metadata",
      [this, patch = std::move(patch)](library::LibraryWrite& transaction, std::span<TrackId const> trackIds)
        -> Result<OperationOutcome<UpdateTrackMetadataReply>>
      {
        auto replyRes = applyMetadataPatchInTransaction(library, transaction, trackIds, patch);

        if (!replyRes)
        {
          return std::unexpected{replyRes.error()};
        }

        auto reply = std::move(*replyRes);

        if (reply.changes.empty())
        {
          return Unchanged<UpdateTrackMetadataReply>{.value = std::move(reply)};
        }

        auto mutatedIds =
          reply.changes | std::views::transform(&TrackChangeRecord::trackId) | std::ranges::to<std::vector>();
        return Changed<UpdateTrackMetadataReply>{
          .value = std::move(reply),
          .changeSet = LibraryChangeSet{.tracksMutated = std::move(mutatedIds)},
        };
      });
  }

  async::Task<Result<EditTrackTagsReply>> LibraryCommands::Impl::previewEditTags(
    LibraryWriteLane::Submission submission,
    std::vector<TrackId> trackIds,
    std::vector<std::string> tagsToAdd,
    std::vector<std::string> tagsToRemove)
  {
    return detail::applyInteractivePreviewAsync(
      std::move(submission),
      [this, trackIds = std::move(trackIds), tagsToAdd = std::move(tagsToAdd), tagsToRemove = std::move(tagsToRemove)](
        library::LibraryWrite& transaction)
      { return detail::applyTagPatchInTransaction(library, transaction, trackIds, tagsToAdd, tagsToRemove); });
  }

  async::Task<Result<TrackAuthoringResult<EditTrackTagsReply>>> LibraryCommands::Impl::applyEditTags(
    LibraryWriteLane::Submission submission,
    BoundTrackTargets targets,
    std::vector<std::string> tagsToAdd,
    std::vector<std::string> tagsToRemove)
  {
    return detail::executeBoundTrackAuthoringAsync<EditTrackTagsReply>(
      std::move(submission),
      std::move(targets),
      "Edit track tags",
      [this, tagsToAdd = std::move(tagsToAdd), tagsToRemove = std::move(tagsToRemove)](
        library::LibraryWrite& transaction,
        std::span<TrackId const> trackIds) -> Result<OperationOutcome<EditTrackTagsReply>>
      {
        auto replyRes = detail::applyTagPatchInTransaction(library, transaction, trackIds, tagsToAdd, tagsToRemove);

        if (!replyRes)
        {
          return std::unexpected{replyRes.error()};
        }

        auto reply = std::move(*replyRes);

        if (reply.changes.empty())
        {
          return Unchanged<EditTrackTagsReply>{.value = std::move(reply)};
        }

        auto mutatedIds =
          reply.changes | std::views::transform(&TrackTagsChange::trackId) | std::ranges::to<std::vector>();
        return Changed<EditTrackTagsReply>{
          .value = std::move(reply),
          .changeSet = LibraryChangeSet{.tracksMutated = std::move(mutatedIds)},
        };
      });
  }

  async::Task<Result<TrackAuthoringResult<UpdateTrackPropertiesReply>>> LibraryCommands::Impl::applyUpdateProperties(
    LibraryWriteLane::Submission submission,
    BoundTrackTargets targets,
    TrackPropertiesPatch patch)
  {
    return detail::executeBoundTrackAuthoringAsync<UpdateTrackPropertiesReply>(
      std::move(submission),
      std::move(targets),
      "Update track properties",
      [this, patch = std::move(patch)](library::LibraryWrite& transaction, std::span<TrackId const> trackIds)
        -> Result<OperationOutcome<UpdateTrackPropertiesReply>>
      {
        auto replyRes = applyPropertiesPatchInTransaction(library, transaction, trackIds, patch);

        if (!replyRes)
        {
          return std::unexpected{replyRes.error()};
        }

        auto reply = std::move(*replyRes);
        auto mutatedIds = std::vector<TrackId>{};
        mutatedIds.reserve(reply.metadata.changes.size() + reply.tags.changes.size());

        for (auto const& change : reply.metadata.changes)
        {
          mutatedIds.push_back(change.trackId);
        }

        for (auto const& change : reply.tags.changes)
        {
          mutatedIds.push_back(change.trackId);
        }

        if (mutatedIds.empty())
        {
          return Unchanged<UpdateTrackPropertiesReply>{.value = std::move(reply)};
        }

        std::ranges::sort(mutatedIds);
        auto const uniqueEnd = std::ranges::unique(mutatedIds).begin();
        mutatedIds.erase(uniqueEnd, mutatedIds.end());

        return Changed<UpdateTrackPropertiesReply>{
          .value = std::move(reply),
          .changeSet = LibraryChangeSet{.tracksMutated = std::move(mutatedIds)},
        };
      });
  }
} // namespace ao::rt
