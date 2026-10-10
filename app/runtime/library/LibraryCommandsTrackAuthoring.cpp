// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "runtime/library/LibraryCommandsInternal.h"
#include "runtime/library/LibraryWriteLane.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Task.h>
#include <ao/library/Credits.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/RecordingDate.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryCommands.h>

#include <boost/unordered/unordered_flat_set.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
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

    Result<> normalizeCustomMetadataUpdates(MetadataPatch& normalized)
    {
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
          // Deletion remains available to clean up an existing reserved custom key.
          if (isReservedCustomMetadataKey(*keyRes))
          {
            return makeError(Error::Code::InvalidInput, std::format("Custom metadata key '{}' is reserved", *keyRes));
          }

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

      return {};
    }

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
        &MetadataPatch::optWork,
        &MetadataPatch::optMovement,
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

      if (normalized.optRecordingDate && !normalized.optRecordingDate->isValid())
      {
        return makeError(Error::Code::InvalidInput, "Track recording date is not a valid partial date");
      }

      if (normalized.optCredits)
      {
        auto& replacement = *normalized.optCredits;

        if (replacement.kinds.none())
        {
          return makeError(Error::Code::InvalidInput, "Credits replacement requires a nonempty scope");
        }

        for (auto const& entry : replacement.entries)
        {
          if (!library::isValidCreditKind(entry.kind) || !replacement.kinds.test(static_cast<std::size_t>(entry.kind)))
          {
            return makeError(Error::Code::InvalidInput, "Credit kind is invalid or outside the replacement scope");
          }
        }

        auto creditsRes = library::normalizeCredits(std::span<library::Credit const>{replacement.entries});

        if (!creditsRes)
        {
          return std::unexpected{creditsRes.error()};
        }

        replacement.entries = std::move(*creditsRes);
      }

      if (auto customRes = normalizeCustomMetadataUpdates(normalized); !customRes)
      {
        return std::unexpected{customRes.error()};
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

    void applyRecordingDatePatch(library::TrackBuilder::MetadataBuilder& metadata,
                                 MetadataPatch const& patch,
                                 PatchResult& result,
                                 std::vector<TrackFieldChange>& changes)
    {
      if (!patch.optRecordingDate || metadata.recordingDate() == *patch.optRecordingDate)
      {
        return;
      }

      changes.push_back(TrackFieldChange{.field = "recordingDate",
                                         .oldValue = library::formatRecordingDate(metadata.recordingDate()),
                                         .newValue = library::formatRecordingDate(*patch.optRecordingDate)});
      metadata.recordingDate(*patch.optRecordingDate);
      result.changedCold = true;
    }

    // Diagnostics only: credit names and roles may contain these separators.
    // This readable report is never parsed into an edit or retained as Undo.
    std::string formatCreditsForChangeReport(std::span<library::CreditView const> credits)
    {
      auto text = std::string{};

      for (auto const& credit : credits)
      {
        if (!text.empty())
        {
          text.push_back('\n');
        }

        text.append(library::creditKindToken(credit.kind));
        text.append(": ");
        text.append(credit.name);

        if (!credit.role.empty())
        {
          text.append(" (");
          text.append(credit.role);
          text.push_back(')');
        }
      }

      return text;
    }

    void applyCreditsPatch(library::TrackBuilder::MetadataBuilder& metadata,
                           MetadataPatch const& patch,
                           PatchResult& result,
                           std::vector<TrackFieldChange>& changes)
    {
      if (!patch.optCredits)
      {
        return;
      }

      auto const& replacement = *patch.optCredits;
      auto const current = metadata.credits();
      auto merged = std::vector<library::CreditView>{};
      merged.reserve(current.size() + replacement.entries.size());

      for (std::size_t kindIndex = 0; kindIndex < library::kCreditKindCount; ++kindIndex)
      {
        auto const kind = static_cast<library::CreditKind>(kindIndex);

        if (replacement.kinds.test(kindIndex))
        {
          for (auto const& entry : replacement.entries)
          {
            if (entry.kind == kind)
            {
              merged.push_back({.name = entry.name, .kind = entry.kind, .role = entry.role});
            }
          }
        }
        else
        {
          for (auto const& entry : current)
          {
            if (entry.kind == kind)
            {
              merged.push_back(entry);
            }
          }
        }
      }

      auto const sameList =
        current.size() == merged.size() &&
        std::ranges::equal(current,
                           merged,
                           [](auto const& lhs, auto const& rhs)
                           { return lhs.name == rhs.name && lhs.kind == rhs.kind && lhs.role == rhs.role; });

      if (sameList)
      {
        return;
      }

      changes.push_back(TrackFieldChange{
        .field = std::string{kCreditsMetadataKey},
        .oldValue = formatCreditsForChangeReport(current),
        .newValue = formatCreditsForChangeReport(merged),
      });
      metadata.credits(merged);
      result.changedCold = true;
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
      applyRecordingDatePatch(metadata, patch, result, changes);
      applyCreditsPatch(metadata, patch, result, changes);
      applyCustomMetadataPatch(builder, patch, result, changes);

      return result;
    }

    /**
     * @brief Writes exactly the halves @p patchResult says changed.
     *
     * Reports whether anything was written: a track whose patch changed nothing
     * is not written at all, and a patch touching one half does not rewrite the
     * other. @p action names the operation in a storage error.
     */
    Result<bool> writeChangedTrackHalves(library::TrackWriter& writer,
                                         TrackId const trackId,
                                         library::TrackBuilder& builder,
                                         PatchResult const& patchResult,
                                         char const* const action)
    {
      if (!patchResult.changedHot && !patchResult.changedCold)
      {
        return false;
      }

      auto updateRes = Result<>{};

      if (patchResult.changedHot && patchResult.changedCold)
      {
        updateRes = writer.update(trackId, builder);
      }
      else if (patchResult.changedHot)
      {
        updateRes = writer.updateHot(trackId, builder);
      }
      else
      {
        updateRes = writer.updateCold(trackId, builder);
      }

      if (!updateRes)
      {
        return detail::storageError(action, updateRes.error());
      }

      return true;
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
      auto seenTrackIds = boost::unordered_flat_set<TrackId, std::hash<TrackId>>{};
      seenTrackIds.reserve(trackIds.size());

      for (auto const trackId : trackIds)
      {
        // Do not reload dictionary IDs staged by an earlier write of this track.
        if (!seenTrackIds.insert(trackId).second)
        {
          continue;
        }

        auto optView = writer.get(trackId, library::TrackStore::Reader::LoadMode::Both);

        if (!optView)
        {
          continue;
        }

        auto builder = library::TrackBuilder::fromCompleteView(*optView, library.dictionary());
        auto fieldChanges = std::vector<TrackFieldChange>{};
        auto const patchRes = applyMetadataPatch(builder, normalizedPatch, fieldChanges);
        auto const wroteRes =
          writeChangedTrackHalves(writer, trackId, builder, patchRes, "Failed to update track data");

        if (!wroteRes)
        {
          return std::unexpected{wroteRes.error()};
        }

        if (!*wroteRes)
        {
          continue;
        }

        changes.push_back(TrackChangeRecord{.trackId = trackId, .fields = std::move(fieldChanges)});
      }

      return UpdateTrackMetadataReply{.changes = std::move(changes)};
    }

    bool tryApplyTrackTagChanges(library::TrackBuilder& builder,
                                 std::span<std::string const> const normalizedAdd,
                                 std::span<std::string const> const normalizedRemove,
                                 std::vector<std::string>& addedTags,
                                 std::vector<std::string>& removedTags)
    {
      bool tagsChanged = false;

      for (auto const& tag : normalizedAdd)
      {
        if (!std::ranges::contains(builder.tags().names(), tag))
        {
          builder.tags().add(tag);
          addedTags.push_back(tag);
          tagsChanged = true;
        }
      }

      for (auto const& tag : normalizedRemove)
      {
        if (std::ranges::contains(builder.tags().names(), tag))
        {
          builder.tags().remove(tag);
          removedTags.push_back(tag);
          tagsChanged = true;
        }
      }

      return tagsChanged;
    }

    Result<UpdateTrackPropertiesReply> applyPropertiesPatchInTransaction(library::MusicLibrary& library,
                                                                         library::LibraryWrite& transaction,
                                                                         std::span<TrackId const> trackIds,
                                                                         TrackPropertiesPatch const& patch)
    {
      auto normalizedPatchRes = normalizeMetadataPatch(patch.metadata);

      if (!normalizedPatchRes)
      {
        return std::unexpected{normalizedPatchRes.error()};
      }

      auto normalizedAddRes = detail::normalizeTags(patch.tagsToAdd);

      if (!normalizedAddRes)
      {
        return std::unexpected{normalizedAddRes.error()};
      }

      auto normalizedRemoveRes = detail::normalizeTags(patch.tagsToRemove);

      if (!normalizedRemoveRes)
      {
        return std::unexpected{normalizedRemoveRes.error()};
      }

      if (auto const disjointRes = detail::validateDisjointTags(*normalizedAddRes, *normalizedRemoveRes); !disjointRes)
      {
        return std::unexpected{disjointRes.error()};
      }

      auto const& normalizedPatch = *normalizedPatchRes;
      auto const& normalizedAdd = *normalizedAddRes;
      auto const& normalizedRemove = *normalizedRemoveRes;

      auto writer = transaction.tracks();
      auto metadataChanges = std::vector<TrackChangeRecord>{};
      auto tagChanges = std::vector<TrackTagsChange>{};
      auto seenTrackIds = boost::unordered_flat_set<TrackId, std::hash<TrackId>>{};
      seenTrackIds.reserve(trackIds.size());

      for (auto const trackId : trackIds)
      {
        // Do not reload dictionary IDs staged by an earlier write of this track.
        if (!seenTrackIds.insert(trackId).second)
        {
          continue;
        }

        auto optView = writer.get(trackId, library::TrackStore::Reader::LoadMode::Both);

        if (!optView)
        {
          continue;
        }

        auto builder = library::TrackBuilder::fromCompleteView(*optView, library.dictionary());
        auto fieldChanges = std::vector<TrackFieldChange>{};
        auto patchRes = applyMetadataPatch(builder, normalizedPatch, fieldChanges);

        auto addedTags = std::vector<std::string>{};
        auto removedTags = std::vector<std::string>{};
        bool const tagsChanged =
          tryApplyTrackTagChanges(builder, normalizedAdd, normalizedRemove, addedTags, removedTags);

        if (tagsChanged)
        {
          patchRes.changedHot = true;
        }

        auto const wroteRes =
          writeChangedTrackHalves(writer, trackId, builder, patchRes, "Failed to update track properties");

        if (!wroteRes)
        {
          return std::unexpected{wroteRes.error()};
        }

        if (!*wroteRes)
        {
          continue;
        }

        if (!fieldChanges.empty())
        {
          metadataChanges.push_back(TrackChangeRecord{.trackId = trackId, .fields = std::move(fieldChanges)});
        }

        if (tagsChanged)
        {
          tagChanges.push_back(TrackTagsChange{
            .trackId = trackId,
            .addedTags = std::move(addedTags),
            .removedTags = std::move(removedTags),
          });
        }
      }

      return UpdateTrackPropertiesReply{
        .metadata = UpdateTrackMetadataReply{.changes = std::move(metadataChanges)},
        .tags = EditTrackTagsReply{.changes = std::move(tagChanges)},
      };
    }
  } // namespace

  async::Task<Result<UpdateTrackMetadataReply>> LibraryCommands::Impl::previewUpdateMetadataAsync(
    LibraryWriteLane::Submission submission,
    std::vector<TrackId> trackIds,
    MetadataPatch patch)
  {
    return detail::applyInteractivePreviewAsync(
      std::move(submission),
      [this, trackIds = std::move(trackIds), patch = std::move(patch)](library::LibraryWrite& transaction)
      { return applyMetadataPatchInTransaction(library, transaction, trackIds, patch); });
  }

  async::Task<Result<TrackAuthoringResult<UpdateTrackMetadataReply>>> LibraryCommands::Impl::applyUpdateMetadataAsync(
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

  async::Task<Result<EditTrackTagsReply>> LibraryCommands::Impl::previewEditTagsAsync(
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

  async::Task<Result<TrackAuthoringResult<EditTrackTagsReply>>> LibraryCommands::Impl::applyEditTagsAsync(
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

  async::Task<Result<TrackAuthoringResult<UpdateTrackPropertiesReply>>>
  LibraryCommands::Impl::applyUpdatePropertiesAsync(LibraryWriteLane::Submission submission,
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
