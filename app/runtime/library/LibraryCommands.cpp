// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/library/LibraryCommands.h>

#include "LibraryWriteLane.h"
#include "MediaTrack.h"
#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/OperationCancelled.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/LibraryUri.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListStore.h>
#include <ao/library/ListView.h>
#include <ao/library/ListWriter.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackWriter.h>
#include <ao/query/Field.h>
#include <ao/query/Parser.h>
#include <ao/query/PlanEvaluator.h>
#include <ao/query/QueryCompilation.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/TrackEditScript.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/WritableTagList.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/utility/Path.h>
#include <ao/utility/StrongTypeFormatter.h>
#include <ao/utility/UnicodeText.h>

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
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
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

    Result<std::string> normalizeRuntimeText(std::string_view const value, std::string_view const context)
    {
      auto normalizedRes = utility::normalizeUtf8Nfc(value);

      if (!normalizedRes)
      {
        auto error = std::move(normalizedRes.error());
        error.message = std::format("{}: {}", context, error.message);
        return std::unexpected{std::move(error)};
      }

      return std::move(*normalizedRes);
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

        auto valueRes = normalizeRuntimeText(*optValue, "Track metadata");

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

        auto keyRes = normalizeRuntimeText(key, "Custom metadata key");

        if (!keyRes)
        {
          return std::unexpected{keyRes.error()};
        }

        auto optNormalizedValue = std::optional<std::string>{};

        if (optValue)
        {
          auto valueRes = normalizeRuntimeText(*optValue, "Custom metadata value");

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

    Result<std::vector<std::string>> normalizeTags(std::span<std::string const> const tags)
    {
      auto normalized = std::vector<std::string>{};
      normalized.reserve(tags.size());
      auto seen = std::unordered_set<std::string>{};
      seen.reserve(tags.size());

      for (auto const& tag : tags)
      {
        auto tagRes = normalizeRuntimeText(tag, "Track tag");

        if (!tagRes)
        {
          return std::unexpected{tagRes.error()};
        }

        if (seen.insert(*tagRes).second)
        {
          normalized.push_back(std::move(*tagRes));
        }
      }

      return normalized;
    }

    Result<ListDraft> normalizeListDraft(ListDraft const& draft)
    {
      auto normalized = draft;
      auto nameRes = normalizeRuntimeText(draft.name, "List name");

      if (!nameRes)
      {
        return std::unexpected{nameRes.error()};
      }

      auto descriptionRes = normalizeRuntimeText(draft.description, "List description");

      if (!descriptionRes)
      {
        return std::unexpected{descriptionRes.error()};
      }

      normalized.name = std::move(*nameRes);
      normalized.description = std::move(*descriptionRes);
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

    std::unexpected<Error> storageError(char const* action, Error const& error)
    {
      return std::unexpected{Error{
        .code = error.code,
        .message = std::format("{}: {}", action, error.message),
        .location = error.location,
      }};
    }

    std::unexpected<Error> prefixError(char const* prefix, Error const& error)
    {
      return std::unexpected{Error{
        .code = error.code,
        .message = std::format("{}: {}", prefix, error.message),
        .location = error.location,
      }};
    }

    template<typename Operation,
             typename OperationResult = std::remove_cvref_t<std::invoke_result_t<Operation, library::LibraryWrite&>>>
    async::Task<OperationResult> applyInteractivePreviewAsync(LibraryWriteLane::Submission submission,
                                                              Operation operation)
    {
      auto mutationRes = co_await LibraryWriteLane::beginInteractiveMutationAsync(std::move(submission));

      if (!mutationRes)
      {
        co_return std::unexpected{mutationRes.error()};
      }

      auto mutation = std::move(*mutationRes);
      auto result = mutation.apply(std::move(operation));
      mutation.abort();
      co_return result;
    }

    template<typename Operation,
             typename OperationResult = std::remove_cvref_t<std::invoke_result_t<Operation, library::LibraryWrite&>>,
             typename Value = detail::OperationResultTraits<OperationResult>::ValueType>
    async::Task<Result<MutationExecution<Value>>> executeInteractiveMutationAsync(
      LibraryWriteLane::Submission submission,
      std::string operationName,
      Operation operation)
    {
      auto mutationRes = co_await LibraryWriteLane::beginInteractiveMutationAsync(std::move(submission));

      if (!mutationRes)
      {
        co_return std::unexpected{mutationRes.error()};
      }

      auto mutation = std::move(*mutationRes);
      auto executionRes = co_await mutation.executeAsync(std::move(operation), std::move(operationName));
      co_return std::move(executionRes);
    }

    template<typename Reply, typename Operation>
    async::Task<Result<TrackAuthoringResult<Reply>>> executeBoundTrackAuthoringAsync(
      LibraryWriteLane::Submission submission,
      BoundTrackTargets targets,
      std::string operationName,
      Operation operation)
    {
      auto start = co_await LibraryWriteLane::beginAuthoringMutationAsync(std::move(submission), targets);
      auto result = TrackAuthoringResult<Reply>{.status = start.status};

      if (!start.optMutation)
      {
        co_return result;
      }

      auto executionRes = co_await start.optMutation->executeAsync(
        [&targets, operation = std::move(operation)](
          library::LibraryWrite& transaction) mutable -> Result<OperationOutcome<Reply>>
        { return operation(transaction, targets.trackIds()); },
        std::move(operationName));

      if (!executionRes)
      {
        co_return std::unexpected{executionRes.error()};
      }

      result.reply = std::move(executionRes->value);

      if (!executionRes->optCommittedRevision)
      {
        result.status = AuthoringStatus::NoOp;
        co_return result;
      }

      result.status = AuthoringStatus::Applied;
      result.optNextTargets.emplace(
        LibraryWriteLane::advanceBoundTargets(targets, *executionRes->optCommittedRevision));
      co_return result;
    }

    template<typename Reply, typename Operation>
    async::Task<Result<AuthoringResult<Reply>>> executeBoundListOrderAuthoringAsync(
      LibraryWriteLane::Submission submission,
      BoundListOrder order,
      std::string operationName,
      Operation operation)
    {
      auto start = co_await LibraryWriteLane::beginListOrderAuthoringMutationAsync(std::move(submission), order);
      auto result = AuthoringResult<Reply>{.status = start.status};

      if (!start.optMutation)
      {
        co_return result;
      }

      auto executionRes = co_await start.optMutation->executeAsync(
        [&order, operation = std::move(operation)](
          library::LibraryWrite& transaction) mutable -> Result<OperationOutcome<Reply>>
        { return operation(transaction, order); },
        std::move(operationName));

      if (!executionRes)
      {
        co_return std::unexpected{executionRes.error()};
      }

      result.reply = std::move(executionRes->value);
      result.status = executionRes->optCommittedRevision ? AuthoringStatus::Applied : AuthoringStatus::NoOp;
      co_return result;
    }

    template<typename Reply>
    struct ChangedWork final
    {
      Reply reply{};
      LibraryChangeSet changeSet{};
    };

    template<typename Reply, typename Operation>
    async::Task<Result<Reply>> executeChangedWorkAsync(LibraryWriteLane::Submission submission,
                                                       std::string operationName,
                                                       Operation operation)
    {
      auto executionRes = co_await executeInteractiveMutationAsync(
        std::move(submission),
        std::move(operationName),
        [operation =
           std::move(operation)](library::LibraryWrite& transaction) mutable -> Result<OperationOutcome<Reply>>
        {
          auto workRes = operation(transaction);

          if (!workRes)
          {
            return std::unexpected{workRes.error()};
          }

          auto work = std::move(*workRes);
          return Changed<Reply>{
            .value = std::move(work.reply),
            .changeSet = std::move(work.changeSet),
          };
        });

      if (!executionRes)
      {
        co_return std::unexpected{executionRes.error()};
      }

      co_return std::move(executionRes->value);
    }

    template<typename Reply, typename Operation>
    async::Task<Result<Reply>> previewChangedWorkAsync(LibraryWriteLane::Submission submission, Operation operation)
    {
      auto workRes = co_await applyInteractivePreviewAsync(std::move(submission), std::move(operation));

      if (!workRes)
      {
        co_return std::unexpected{workRes.error()};
      }

      co_return std::move(workRes->reply);
    }

    template<typename Value, typename Owner, typename Operation>
    async::Task<Value> runWriterOperation(std::shared_ptr<Owner> ownerPtr,
                                          LibraryWriteLane::Submission submission,
                                          Operation operation)
    {
      auto optResult = std::optional<Value>{};
      auto deferredException = std::exception_ptr{};

      try
      {
        optResult.emplace(co_await operation(*ownerPtr, std::move(submission)));
      }
      catch (...)
      {
        deferredException = std::current_exception();
      }

      co_await ownerPtr->asyncRuntime.resumeOnCallbackExecutor();

      if (deferredException)
      {
        async::rethrowException(deferredException);
      }

      AO_INVARIANT(optResult);
      co_return std::move(*optResult);
    }

    // This ordinary function captures submission context before the returned
    // lazy coroutine can outlive its caller or cross an executor boundary.
    template<typename Value, typename Owner, typename Method, typename... Args>
    async::Task<Value> submitWriterOperation(std::shared_ptr<Owner> ownerPtr, Method method, Args... args)
    {
      auto submission = ownerPtr->writeLane.captureSubmission();
      return runWriterOperation<Value>(
        std::move(ownerPtr),
        std::move(submission),
        [method, ... args = std::move(args)](Owner& owner, LibraryWriteLane::Submission innerSubmission) mutable
        { return std::invoke(method, owner, std::move(innerSubmission), std::move(args)...); });
    }

    struct ImportTarget final
    {
      std::filesystem::path fullPath;
      std::string uri;
    };

    Result<ImportTarget> importTargetForPath(library::MusicLibrary const& library, std::filesystem::path const& path)
    {
      auto ec = std::error_code{};
      auto const root = std::filesystem::weakly_canonical(library.rootPath(), ec);

      if (ec)
      {
        return makeError(
          Error::Code::IoError,
          std::format("failed to resolve music root '{}': {}", utility::pathToUtf8(library.rootPath()), ec.message()));
      }

      bool sawOutsideRoot = false;

      auto const resolveInsideRoot =
        [&root, &sawOutsideRoot](std::filesystem::path const& candidate) -> std::optional<ImportTarget>
      {
        auto ec = std::error_code{};
        auto fullPath = std::filesystem::weakly_canonical(candidate, ec);

        if (ec)
        {
          return std::nullopt;
        }

        auto const rel = fullPath.lexically_relative(root);

        auto uriRes = library::LibraryUri::parse(utility::pathToGenericUtf8(rel));

        if (!uriRes)
        {
          sawOutsideRoot = true;
          return std::nullopt;
        }

        auto resolvedPathRes = uriRes->resolveUnder(root);

        if (!resolvedPathRes)
        {
          sawOutsideRoot = true;
          return std::nullopt;
        }

        if (!std::filesystem::is_regular_file(*resolvedPathRes, ec) || ec)
        {
          return std::nullopt;
        }

        return ImportTarget{.fullPath = std::move(*resolvedPathRes), .uri = std::string{uriRes->value()}};
      };

      auto optTarget = resolveInsideRoot(path.is_absolute() ? path : root / path);

      if (!optTarget)
      {
        if (sawOutsideRoot)
        {
          return makeError(
            Error::Code::InvalidInput, std::format("track file is outside music root: {}", utility::pathToUtf8(path)));
        }

        return makeError(
          Error::Code::NotFound, std::format("track file not found under music root: {}", utility::pathToUtf8(path)));
      }

      return std::move(*optTarget);
    }

    struct PreparedTrackImport final
    {
      ImportTarget target;
      MediaTrack mediaTrack;
    };

    Result<PreparedTrackImport> prepareTrackImport(library::MusicLibrary const& library,
                                                   std::filesystem::path const& path)
    {
      auto targetRes = importTargetForPath(library, path);

      if (!targetRes)
      {
        return std::unexpected{targetRes.error()};
      }

      auto mediaTrackRes = readMediaTrack(targetRes->fullPath);

      if (!mediaTrackRes)
      {
        return std::unexpected{mediaTrackRes.error()};
      }

      return PreparedTrackImport{.target = std::move(*targetRes), .mediaTrack = std::move(*mediaTrackRes)};
    }

    Result<> validateListExpression(std::string const& expression)
    {
      auto exprRes = query::parse(expression.empty() ? "true" : expression);

      if (!exprRes)
      {
        return prefixError("invalid list filter", exprRes.error());
      }

      if (auto planRes = query::compileQuery(*exprRes); !planRes)
      {
        return prefixError("invalid list filter", planRes.error());
      }

      return {};
    }

    library::ListBuilder listForDraft(ListDraft const& draft,
                                      std::optional<library::ListView> const& optExisting = std::nullopt)
    {
      auto builder = optExisting ? library::ListBuilder::fromView(*optExisting) : library::ListBuilder::makeEmpty();
      builder.name(draft.name).description(draft.description).filter(draft.expression).parentId(draft.parentId);
      return builder;
    }

    Result<> validateListDraft(ListDraft const& draft)
    {
      return validateListExpression(draft.expression);
    }

    std::vector<TrackId> orderTrackIdsFrom(library::ListView::OrderTrackIdProxy const& trackIds)
    {
      auto result = std::vector<TrackId>{};
      result.reserve(trackIds.size());
      result.append_range(trackIds);
      return result;
    }

    std::vector<TrackId> orderTrackIdsFrom(library::ListView const& view)
    {
      return orderTrackIdsFrom(view.orderTrackIds());
    }

    library::ListBuilder listWithOrder(library::ListView const& view, std::span<TrackId const> orderTrackIds)
    {
      auto builder = library::ListBuilder::fromView(view);
      builder.orderTrackIds().clear();

      for (auto const trackId : orderTrackIds)
      {
        builder.orderTrackIds().add(trackId);
      }

      return builder;
    }

    Result<library::ListView> requireList(library::ListWriter const& listWriter, ListId listId)
    {
      auto optView = listWriter.get(listId);

      if (!optView)
      {
        return makeError(Error::Code::NotFound, std::format("list not found: {}", listId));
      }

      return *optView;
    }

    delta::RegularTrackEditScript removalScriptFor(std::span<TrackId const> storedTrackIds,
                                                   std::unordered_set<TrackId> const& selectedTrackIds)
    {
      auto removals = std::vector<delta::RemoveRange>{};

      for (std::size_t index = 0; index < storedTrackIds.size(); ++index)
      {
        auto const trackId = storedTrackIds[index];

        if (!selectedTrackIds.contains(trackId))
        {
          continue;
        }

        if (removals.empty() || removals.back().start + removals.back().trackIds.size() != index)
        {
          removals.push_back(delta::RemoveRange{.start = index});
        }

        removals.back().trackIds.push_back(trackId);
      }

      std::ranges::reverse(removals);
      auto script = delta::RegularTrackEditScript{};
      script.edits.reserve(removals.size());

      for (auto& removal : removals)
      {
        script.edits.emplace_back(std::move(removal));
      }

      return script;
    }

    struct ListOrderRemovalResult final
    {
      std::vector<ListId> changedListIds{};
      std::vector<ListOrderChange> orderChanges{};
    };

    struct PendingListOrderRemoval final
    {
      ListId listId = kInvalidListId;
      library::ListBuilder list;
      ListOrderChange orderChange{};
    };

    Result<ListOrderRemovalResult> removeTrackFromListOrders(library::MusicLibrary& library,
                                                             library::LibraryWrite& transaction,
                                                             TrackId trackId)
    {
      auto updates = std::vector<PendingListOrderRemoval>{};

      {
        auto listReader = library.lists().reader(transaction);

        for (auto const& [listId, view] : listReader)
        {
          if (!std::ranges::contains(view.orderTrackIds(), trackId))
          {
            continue;
          }

          auto const storedTrackIds = orderTrackIdsFrom(view);
          auto const selectedTrackIds = std::unordered_set<TrackId>{trackId};
          auto builder = library::ListBuilder::fromView(view);
          builder.orderTrackIds().remove(trackId);

          updates.push_back(PendingListOrderRemoval{
            .listId = listId,
            .list = std::move(builder),
            .orderChange =
              ListOrderChange{
                .listId = listId,
                .operation = removalScriptFor(storedTrackIds, selectedTrackIds),
              },
          });
        }
      }

      auto listWriter = transaction.lists();
      auto result = ListOrderRemovalResult{};
      result.changedListIds.reserve(updates.size());
      result.orderChanges.reserve(updates.size());

      for (auto& update : updates)
      {
        if (auto updateRes = listWriter.update(update.listId, update.list); !updateRes)
        {
          return storageError("Failed to remove deleted track from List order", updateRes.error());
        }

        result.changedListIds.push_back(update.listId);
        result.orderChanges.push_back(std::move(update.orderChange));
      }

      return result;
    }

    void appendListFieldChange(std::vector<ListFieldChange>& changes,
                               std::string_view field,
                               std::string_view oldValue,
                               std::string_view newValue)
    {
      if (oldValue == newValue)
      {
        return;
      }

      changes.push_back(ListFieldChange{
        .field = std::string{field}, .oldValue = std::string{oldValue}, .newValue = std::string{newValue}});
    }

    UpdateListReply diffListUpdate(library::ListView const& existing, ListDraft const& draft)
    {
      auto reply = UpdateListReply{};
      appendListFieldChange(reply.fieldChanges, "name", existing.name(), draft.name);
      appendListFieldChange(reply.fieldChanges, "description", existing.description(), draft.description);
      appendListFieldChange(reply.fieldChanges,
                            "parentId",
                            std::format("{}", existing.parentId().raw()),
                            std::format("{}", draft.parentId.raw()));
      appendListFieldChange(reply.fieldChanges, "filter", existing.filter(), draft.expression);

      return reply;
    }

    Result<ListId> createListInTransaction(library::LibraryWrite& transaction, ListDraft const& draft)
    {
      auto normalizedDraftRes = normalizeListDraft(draft);

      if (!normalizedDraftRes)
      {
        return std::unexpected{normalizedDraftRes.error()};
      }

      auto const& normalizedDraft = *normalizedDraftRes;
      auto listWriter = transaction.lists();
      auto list = listForDraft(normalizedDraft);

      if (auto result = validateListDraft(normalizedDraft); !result)
      {
        return std::unexpected{result.error()};
      }

      auto result = listWriter.create(list);

      if (!result)
      {
        return storageError("Failed to create list", result.error());
      }

      return *result;
    }

    Result<UpdateListReply> updateListInTransaction(library::LibraryWrite& transaction, ListDraft const& draft)
    {
      auto normalizedDraftRes = normalizeListDraft(draft);

      if (!normalizedDraftRes)
      {
        return std::unexpected{normalizedDraftRes.error()};
      }

      auto const& normalizedDraft = *normalizedDraftRes;
      auto listWriter = transaction.lists();
      auto optExisting = listWriter.get(normalizedDraft.listId);

      if (!optExisting)
      {
        return makeError(Error::Code::NotFound, std::format("list not found: {}", normalizedDraft.listId));
      }

      auto list = listForDraft(normalizedDraft, optExisting);

      if (auto result = validateListDraft(normalizedDraft); !result)
      {
        return std::unexpected{result.error()};
      }

      auto reply = diffListUpdate(*optExisting, normalizedDraft);

      if (reply.fieldChanges.empty())
      {
        return reply;
      }

      reply.changed = true;

      if (auto result = listWriter.update(normalizedDraft.listId, list); !result)
      {
        return storageError("Failed to update list", result.error());
      }

      return reply;
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
          return storageError("Failed to update track data", updateRes.error());
        }

        changes.push_back(TrackChangeRecord{.trackId = trackId, .fields = std::move(fieldChanges)});
      }

      return UpdateTrackMetadataReply{.changes = std::move(changes)};
    }

    Result<EditTrackTagsReply> applyTagPatchInTransaction(library::MusicLibrary& library,
                                                          library::LibraryWrite& transaction,
                                                          std::span<TrackId const> trackIds,
                                                          std::span<std::string const> tagsToAdd,
                                                          std::span<std::string const> tagsToRemove)
    {
      auto normalizedAddRes = normalizeTags(tagsToAdd);

      if (!normalizedAddRes)
      {
        return std::unexpected{normalizedAddRes.error()};
      }

      auto normalizedRemoveRes = normalizeTags(tagsToRemove);

      if (!normalizedRemoveRes)
      {
        return std::unexpected{normalizedRemoveRes.error()};
      }

      auto writer = transaction.tracks();
      auto changes = std::vector<TrackTagsChange>{};

      for (auto const trackId : trackIds)
      {
        auto optView = writer.get(trackId, library::TrackStore::Reader::LoadMode::Hot);

        if (!optView)
        {
          continue;
        }

        auto builder = library::TrackBuilder::fromHotView(*optView, library.dictionary());
        auto& tags = builder.tags();
        bool changed = false;
        auto addedTags = std::vector<std::string>{};
        auto removedTags = std::vector<std::string>{};

        for (auto const& tag : *normalizedAddRes)
        {
          if (!std::ranges::contains(tags.names(), tag))
          {
            tags.add(tag);
            addedTags.push_back(tag);
            changed = true;
          }
        }

        for (auto const& tag : *normalizedRemoveRes)
        {
          if (std::ranges::contains(tags.names(), tag))
          {
            tags.remove(tag);
            removedTags.push_back(tag);
            changed = true;
          }
        }

        if (!changed)
        {
          continue;
        }

        if (auto result = writer.updateHot(trackId, builder); !result)
        {
          return storageError("Failed to update hot track data", result.error());
        }

        changes.push_back(TrackTagsChange{
          .trackId = trackId, .addedTags = std::move(addedTags), .removedTags = std::move(removedTags)});
      }

      return EditTrackTagsReply{.changes = std::move(changes)};
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

      auto tagsRes = applyTagPatchInTransaction(library, transaction, trackIds, patch.tagsToAdd, patch.tagsToRemove);

      if (!tagsRes)
      {
        return std::unexpected{tagsRes.error()};
      }

      return UpdateTrackPropertiesReply{.metadata = std::move(*metadataRes), .tags = std::move(*tagsRes)};
    }

    struct DeleteListTagImpactWork final
    {
      DeleteListReply::TagImpact impact{};
      std::vector<TrackId> taggedTrackIds{};
    };

    Result<std::optional<DeleteListTagImpactWork>> analyzeDeleteListTagImpact(
      library::MusicLibrary& library,
      library::LibraryWrite& transaction,
      library::ListView const& targetView,
      std::span<ListId const> const deletedListIds)
    {
      auto const optTag = writableTagForListExpression(targetView.filter());

      if (!optTag)
      {
        return std::optional<DeleteListTagImpactWork>{};
      }

      auto work = DeleteListTagImpactWork{};
      work.impact.tag = *optTag;
      auto const deleted = std::unordered_set<ListId>{deletedListIds.begin(), deletedListIds.end()};

      for (auto const& [listId, view] : library.lists().reader(transaction))
      {
        if (!deleted.contains(listId) && listExpressionReferencesTag(view.filter(), *optTag))
        {
          work.impact.otherListReferences.push_back(
            DeleteListReply::TagReference{.listId = listId, .name = std::string{view.name()}});
        }
      }

      auto const& dictionary = library.dictionary();

      for (auto const& [trackId, view] : library.tracks().reader(transaction).hot())
      {
        bool hasTag = false;

        for (auto const tagId : view.tags())
        {
          if (dictionary.get(tagId) == *optTag)
          {
            hasTag = true;
            break;
          }
        }

        if (hasTag)
        {
          work.taggedTrackIds.push_back(trackId);
        }
      }

      work.impact.taggedTrackCount = work.taggedTrackIds.size();
      return std::optional<DeleteListTagImpactWork>{std::move(work)};
    }

    Result<std::vector<DeleteListReply>> collectDeleteListSubtree(library::MusicLibrary& library,
                                                                  library::LibraryWrite& transaction,
                                                                  ListId const rootListId)
    {
      auto records = std::unordered_map<ListId, DeleteListReply>{};
      auto childIdsByParent = std::unordered_map<ListId, std::vector<ListId>>{};

      for (auto const& [savedListId, view] : library.lists().reader(transaction))
      {
        records.emplace(savedListId,
                        DeleteListReply{
                          .listId = savedListId,
                          .name = std::string{view.name()},
                          .orderTrackIdCount = view.orderTrackIds().size(),
                        });
        childIdsByParent[view.parentId()].push_back(savedListId);
      }

      if (!records.contains(rootListId))
      {
        return makeError(Error::Code::NotFound, std::format("list not found: {}", rootListId));
      }

      auto deletedLists = std::vector<DeleteListReply>{};
      auto visiting = std::unordered_set<ListId>{};
      auto visited = std::unordered_set<ListId>{};
      auto visit = std::function<Result<>(ListId)>{};
      visit = [&](ListId const currentId) -> Result<>
      {
        if (visited.contains(currentId))
        {
          return {};
        }

        if (!visiting.insert(currentId).second)
        {
          return makeError(Error::Code::InvalidState, "List cycle detected while previewing subtree deletion");
        }

        auto const record = records.find(currentId);

        if (record == records.end())
        {
          return makeError(Error::Code::InvalidState, "List disappeared while previewing subtree deletion");
        }

        deletedLists.push_back(record->second);

        if (auto const children = childIdsByParent.find(currentId); children != childIdsByParent.end())
        {
          for (auto const childId : children->second)
          {
            if (auto result = visit(childId); !result)
            {
              return result;
            }
          }
        }

        visiting.erase(currentId);
        visited.insert(currentId);
        return {};
      };

      if (auto result = visit(rootListId); !result)
      {
        return std::unexpected{result.error()};
      }

      return deletedLists;
    }

    Result<std::string> requireWritableListTag(library::ListView const& view, ListId const listId)
    {
      auto optTag = writableTagForListExpression(view.filter());

      if (!optTag)
      {
        return makeError(
          Error::Code::InvalidInput,
          std::format("List {} membership is computed; its expression must be exactly one positive tag", listId));
      }

      return std::move(*optTag);
    }

    struct ParentFilterPlan final
    {
      ListId listId = kInvalidListId;
      query::ExecutionPlan plan{};
    };

    Error storedParentFilterError(ListId const parentId, Error error)
    {
      error.code = Error::Code::FormatRejected;
      error.message = std::format("invalid stored filter for parent List {}: {}", parentId, error.message);
      return error;
    }

    Result<std::vector<ParentFilterPlan>> compileParentFilterPlans(library::MusicLibrary& library,
                                                                   library::LibraryWrite& transaction,
                                                                   library::ListView const& targetView)
    {
      auto plans = std::vector<ParentFilterPlan>{};
      auto reader = library.lists().reader(transaction);
      auto parentId = targetView.parentId();
      auto visited = std::unordered_set<ListId>{};

      while (parentId != kInvalidListId)
      {
        auto const inserted = visited.insert(parentId).second;
        AO_INVARIANT(inserted, "List parent cycle detected while validating membership");

        auto optParent = reader.get(parentId);

        AO_INVARIANT(optParent, "List parent is missing while validating membership");

        if (!optParent->filter().empty())
        {
          auto expressionRes = query::parse(optParent->filter());

          if (!expressionRes)
          {
            return std::unexpected{storedParentFilterError(parentId, std::move(expressionRes).error())};
          }

          auto planRes = query::compileQuery(*expressionRes);

          if (!planRes)
          {
            return std::unexpected{storedParentFilterError(parentId, std::move(planRes).error())};
          }

          plans.push_back(ParentFilterPlan{.listId = parentId, .plan = std::move(*planRes)});
        }

        parentId = optParent->parentId();
      }

      return plans;
    }

    Result<> validateTracksExist(library::MusicLibrary& library,
                                 library::LibraryWrite& transaction,
                                 std::span<TrackId const> trackIds)
    {
      auto reader = library.tracks().reader(transaction);

      for (auto const trackId : trackIds)
      {
        if (!reader.get(trackId, library::TrackStore::Reader::LoadMode::Hot))
        {
          return makeError(Error::Code::NotFound, std::format("track not found: {}", trackId));
        }
      }

      return {};
    }

    Result<> validateParentMembership(library::MusicLibrary& library,
                                      library::LibraryWrite& transaction,
                                      library::ListView const& targetView,
                                      ListId const targetListId,
                                      std::span<TrackId const> trackIds)
    {
      auto plansRes = compileParentFilterPlans(library, transaction, targetView);

      if (!plansRes)
      {
        return std::unexpected{plansRes.error()};
      }

      auto& plans = *plansRes;

      if (plans.empty())
      {
        return validateTracksExist(library, transaction, trackIds);
      }

      auto dictionaryCache = library::DictionaryReadCache{library.dictionary()};
      auto dictionaryContext = library::DictionaryReadContext{dictionaryCache};
      auto bindings = std::vector<query::PlanBinding>{};
      bindings.reserve(plans.size());

      for (auto const& plan : plans)
      {
        bindings.emplace_back(plan.plan, dictionaryContext);
      }

      auto reader = library.tracks().reader(transaction);
      auto evaluator = query::PlanEvaluator{};

      for (auto const trackId : trackIds)
      {
        auto optTrack = reader.get(trackId, library::TrackStore::Reader::LoadMode::Both);

        if (!optTrack)
        {
          return makeError(Error::Code::NotFound, std::format("track not found: {}", trackId));
        }

        for (std::size_t index = 0; index < plans.size(); ++index)
        {
          AO_INVARIANT(query::hasRequiredTrackData(plans[index].plan.accessProfile, *optTrack),
                       "Complete Track {} lacks data required by parent List {}",
                       trackId,
                       plans[index].listId);

          if (!evaluator.matches(bindings[index], *optTrack))
          {
            return makeError(
              Error::Code::InvalidInput,
              std::format(
                "Track {} is outside parent List {} of target List {}", trackId, plans[index].listId, targetListId));
          }
        }
      }

      return {};
    }

    Result<AddTracksToListReply> applyAddTracksToListInTransaction(library::MusicLibrary& library,
                                                                   library::LibraryWrite& transaction,
                                                                   ListId const listId,
                                                                   std::span<TrackId const> trackIds)
    {
      auto listName = std::string{};
      auto tag = std::string{};

      // LMDB values are borrowed from the transaction and may be invalidated by
      // a later write. Materialize every List field used by the reply before
      // applyTagPatchInTransaction performs any database mutation.
      {
        auto listWriter = transaction.lists();
        auto viewRes = requireList(listWriter, listId);

        if (!viewRes)
        {
          return std::unexpected{viewRes.error()};
        }

        auto tagRes = requireWritableListTag(*viewRes, listId);

        if (!tagRes)
        {
          return std::unexpected{tagRes.error()};
        }

        if (auto eligibilityRes = validateParentMembership(library, transaction, *viewRes, listId, trackIds);
            !eligibilityRes)
        {
          return std::unexpected{eligibilityRes.error()};
        }

        listName = std::string{viewRes->name()};
        tag = std::move(*tagRes);
      }

      auto tags = std::array<std::string, 1>{tag};
      auto editRes = applyTagPatchInTransaction(library, transaction, trackIds, tags, {});

      if (!editRes)
      {
        return std::unexpected{editRes.error()};
      }

      return AddTracksToListReply{
        .listId = listId,
        .listName = std::move(listName),
        .tag = std::move(tag),
        .targetTrackIds = {trackIds.begin(), trackIds.end()},
        .tagEdit = std::move(*editRes),
      };
    }

    struct RemoveTracksFromListWork final
    {
      RemoveTracksFromListReply reply{};
      std::optional<ListOrderChange> optOrderChange{};
    };

    Result<RemoveTracksFromListWork> applyRemoveTracksFromListInTransaction(library::MusicLibrary& library,
                                                                            library::LibraryWrite& transaction,
                                                                            ListId const listId,
                                                                            std::span<TrackId const> trackIds)
    {
      if (auto existenceRes = validateTracksExist(library, transaction, trackIds); !existenceRes)
      {
        return std::unexpected{existenceRes.error()};
      }

      auto const selected = std::unordered_set<TrackId>{trackIds.begin(), trackIds.end()};
      auto listName = std::string{};
      auto tag = std::string{};
      auto oldOrder = std::vector<TrackId>{};
      auto nextOrder = std::vector<TrackId>{};
      auto forgotten = std::vector<TrackId>{};
      auto optList = std::optional<library::ListBuilder>{};

      {
        auto listWriter = transaction.lists();
        auto viewRes = requireList(listWriter, listId);

        if (!viewRes)
        {
          return std::unexpected{viewRes.error()};
        }

        auto tagRes = requireWritableListTag(*viewRes, listId);

        if (!tagRes)
        {
          return std::unexpected{tagRes.error()};
        }

        listName = std::string{viewRes->name()};
        tag = std::move(*tagRes);
        oldOrder = orderTrackIdsFrom(*viewRes);
        nextOrder = oldOrder;

        for (auto const trackId : oldOrder)
        {
          if (selected.contains(trackId))
          {
            forgotten.push_back(trackId);
          }
        }

        std::erase_if(nextOrder, [&selected](TrackId const trackId) { return selected.contains(trackId); });

        if (nextOrder != oldOrder)
        {
          optList.emplace(listWithOrder(*viewRes, nextOrder));
        }
      }

      auto tags = std::array<std::string, 1>{tag};
      auto editRes = applyTagPatchInTransaction(library, transaction, trackIds, {}, tags);

      if (!editRes)
      {
        return std::unexpected{editRes.error()};
      }

      auto work = RemoveTracksFromListWork{
        .reply =
          RemoveTracksFromListReply{
            .listId = listId,
            .listName = std::move(listName),
            .tag = std::move(tag),
            .targetTrackIds = {trackIds.begin(), trackIds.end()},
            .tagEdit = std::move(*editRes),
            .forgottenPositionTrackIds = std::move(forgotten),
          },
      };

      if (nextOrder == oldOrder)
      {
        return work;
      }

      AO_INVARIANT(optList, "Changed List order did not retain its semantic update");

      if (auto updateRes = transaction.lists().update(listId, *optList); !updateRes)
      {
        return storageError("Failed to forget removed List positions", updateRes.error());
      }

      work.optOrderChange = ListOrderChange{
        .listId = listId,
        .operation = removalScriptFor(oldOrder, selected),
      };
      return work;
    }

    Result<LibraryChangeSet> applyMoveListOrderInTransaction(library::LibraryWrite& transaction,
                                                             BoundListOrder const& order,
                                                             std::span<TrackId const> selectedTrackIds,
                                                             std::span<TrackId const> desiredEffectiveTrackIds,
                                                             std::optional<TrackId> const optBeforeTrackId)
    {
      auto listWriter = transaction.lists();
      auto viewRes = requireList(listWriter, order.listId());

      if (!viewRes)
      {
        return std::unexpected{viewRes.error()};
      }

      auto const& view = *viewRes;
      auto const oldOrderTrackIds = orderTrackIdsFrom(view);
      auto nextOrderTrackIds = oldOrderTrackIds;
      auto rankedMembership = std::unordered_set<TrackId>{nextOrderTrackIds.begin(), nextOrderTrackIds.end()};
      auto const effectiveTrackIds = order.effectiveTrackIds();
      auto const effectiveMembership = std::unordered_set<TrackId>{effectiveTrackIds.begin(), effectiveTrackIds.end()};
      auto const selectedMembership = std::unordered_set<TrackId>{selectedTrackIds.begin(), selectedTrackIds.end()};

      for (auto const trackId : effectiveTrackIds)
      {
        if (rankedMembership.insert(trackId).second)
        {
          nextOrderTrackIds.push_back(trackId);
        }
      }

      std::erase_if(nextOrderTrackIds,
                    [&selectedMembership](TrackId const trackId) { return selectedMembership.contains(trackId); });
      auto orderInsertion = nextOrderTrackIds.end();

      if (optBeforeTrackId)
      {
        orderInsertion = std::ranges::find(nextOrderTrackIds, *optBeforeTrackId);

        AO_INVARIANT(
          orderInsertion != nextOrderTrackIds.end(), "Bound List order anchor is absent from materialized order");
      }

      nextOrderTrackIds.insert(orderInsertion, selectedTrackIds.begin(), selectedTrackIds.end());

      auto projectedOrder = std::vector<TrackId>{};
      projectedOrder.reserve(effectiveTrackIds.size());

      for (auto const trackId : nextOrderTrackIds)
      {
        if (effectiveMembership.contains(trackId))
        {
          projectedOrder.push_back(trackId);
        }
      }

      AO_INVARIANT(std::ranges::equal(projectedOrder, desiredEffectiveTrackIds),
                   "Materialized List order does not represent the requested move");

      auto list = listWithOrder(view, nextOrderTrackIds);

      if (auto updateRes = listWriter.update(order.listId(), list); !updateRes)
      {
        return storageError("Failed to update List order", updateRes.error());
      }

      auto script = delta::diff(oldOrderTrackIds, nextOrderTrackIds, {}, selectedTrackIds);
      return LibraryChangeSet{.listsUpserted = {order.listId()},
                              .listOrderChanges = {
                                ListOrderChange{.listId = order.listId(), .operation = std::move(script)},
                              }};
    }

    Result<ChangedWork<DeleteListReply>> applyDeleteListInTransaction(library::MusicLibrary& library,
                                                                      library::LibraryWrite& transaction,
                                                                      ListId const listId,
                                                                      DeleteListOptions const options)
    {
      auto listWriter = transaction.lists();
      auto optView = listWriter.get(listId);

      if (!optView)
      {
        return makeError(Error::Code::NotFound, std::format("list not found: {}", listId));
      }

      auto dependentDescriptions = std::string{};

      for (auto const& [dependentId, dependentView] : library.lists().reader(transaction))
      {
        if (dependentView.parentId() == listId)
        {
          if (!dependentDescriptions.empty())
          {
            dependentDescriptions.append(", ");
          }

          dependentDescriptions.append(std::format("{} ({})", dependentView.name(), dependentId));
        }
      }

      if (!dependentDescriptions.empty())
      {
        return makeError(
          Error::Code::Conflict, std::format("List {} has dependent Lists: {}", listId, dependentDescriptions));
      }

      auto work = ChangedWork<DeleteListReply>{
        .reply =
          DeleteListReply{
            .listId = listId,
            .name = std::string{optView->name()},
            .orderTrackIdCount = optView->orderTrackIds().size(),
          },
      };
      auto const deletedListIds = std::array{listId};
      auto tagImpactWorkRes = analyzeDeleteListTagImpact(library, transaction, *optView, deletedListIds);

      if (!tagImpactWorkRes)
      {
        return std::unexpected{tagImpactWorkRes.error()};
      }

      auto optTagImpactWork = std::move(*tagImpactWorkRes);

      if (auto removeRes = listWriter.remove(listId); !removeRes)
      {
        return std::unexpected{removeRes.error()};
      }

      auto mutatedTrackIds = std::vector<TrackId>{};

      if (options.removeWritableTagFromTracks)
      {
        if (!optTagImpactWork)
        {
          return makeError(
            Error::Code::InvalidInput, std::format("List {} does not have directly editable tag membership", listId));
        }

        auto& tagImpactWork = *optTagImpactWork;
        auto const tags = std::array{tagImpactWork.impact.tag};
        auto tagEditRes = applyTagPatchInTransaction(library, transaction, tagImpactWork.taggedTrackIds, {}, tags);

        if (!tagEditRes)
        {
          return std::unexpected{tagEditRes.error()};
        }

        tagImpactWork.impact.removedFromTrackCount = tagEditRes->changes.size();
        mutatedTrackIds =
          tagEditRes->changes | std::views::transform(&TrackTagsChange::trackId) | std::ranges::to<std::vector>();
      }

      if (optTagImpactWork)
      {
        work.reply.optTagImpact = optTagImpactWork->impact;
      }

      work.changeSet = LibraryChangeSet{.tracksMutated = std::move(mutatedTrackIds), .listsDeleted = {listId}};
      return work;
    }

    Result<ChangedWork<DeleteListSubtreeReply>> applyDeleteListSubtreeInTransaction(library::MusicLibrary& library,
                                                                                    library::LibraryWrite& transaction,
                                                                                    ListId const listId,
                                                                                    DeleteListOptions const options)
    {
      auto deletedListsRes = collectDeleteListSubtree(library, transaction, listId);

      if (!deletedListsRes)
      {
        return std::unexpected{deletedListsRes.error()};
      }

      auto work = ChangedWork<DeleteListSubtreeReply>{
        .reply = DeleteListSubtreeReply{.rootListId = listId, .deletedLists = std::move(*deletedListsRes)},
      };
      auto deletedIds =
        work.reply.deletedLists | std::views::transform(&DeleteListReply::listId) | std::ranges::to<std::vector>();
      auto optRootView = library.lists().reader(transaction).get(listId);

      if (!optRootView)
      {
        return makeError(Error::Code::NotFound, std::format("list not found: {}", listId));
      }

      auto tagImpactWorkRes = analyzeDeleteListTagImpact(library, transaction, *optRootView, deletedIds);

      if (!tagImpactWorkRes)
      {
        return std::unexpected{tagImpactWorkRes.error()};
      }

      auto optTagImpactWork = std::move(*tagImpactWorkRes);
      auto listWriter = transaction.lists();
      auto removedIdsRes = listWriter.removeSubtree(listId);

      if (!removedIdsRes)
      {
        return std::unexpected{removedIdsRes.error()};
      }

      AO_INVARIANT(std::ranges::is_permutation(*removedIdsRes, deletedIds),
                   "Logical List subtree membership disagreed with the application snapshot");

      auto mutatedTrackIds = std::vector<TrackId>{};

      if (options.removeWritableTagFromTracks)
      {
        if (!optTagImpactWork)
        {
          return makeError(
            Error::Code::InvalidInput, std::format("List {} does not have directly editable tag membership", listId));
        }

        auto& tagImpactWork = *optTagImpactWork;
        auto const tags = std::array{tagImpactWork.impact.tag};
        auto tagEditRes = applyTagPatchInTransaction(library, transaction, tagImpactWork.taggedTrackIds, {}, tags);

        if (!tagEditRes)
        {
          return std::unexpected{tagEditRes.error()};
        }

        tagImpactWork.impact.removedFromTrackCount = tagEditRes->changes.size();
        mutatedTrackIds =
          tagEditRes->changes | std::views::transform(&TrackTagsChange::trackId) | std::ranges::to<std::vector>();
      }

      if (optTagImpactWork)
      {
        work.reply.deletedLists.front().optTagImpact = optTagImpactWork->impact;
      }

      work.changeSet =
        LibraryChangeSet{.tracksMutated = std::move(mutatedTrackIds), .listsDeleted = std::move(deletedIds)};
      return work;
    }

    Result<ChangedWork<DeleteTrackReply>> applyDeleteTrackInTransaction(library::MusicLibrary& library,
                                                                        library::LibraryWrite& transaction,
                                                                        TrackId const trackId)
    {
      auto writer = transaction.tracks();
      auto optView = writer.get(trackId, library::TrackStore::Reader::LoadMode::Both);

      if (!optView)
      {
        return makeError(Error::Code::NotFound, std::format("track not found: {}", trackId));
      }

      auto const uri = std::string{optView->property().uri()};
      auto const title = std::string{optView->metadata().title()};
      auto changedListsRes = removeTrackFromListOrders(library, transaction, trackId);

      if (!changedListsRes)
      {
        return std::unexpected{changedListsRes.error()};
      }

      auto changedLists = std::move(*changedListsRes);
      auto work = ChangedWork<DeleteTrackReply>{
        .reply =
          DeleteTrackReply{
            .trackId = trackId,
            .uri = uri,
            .title = title,
            .removedFromListIds = changedLists.changedListIds,
          },
      };

      auto removeRes = writer.remove(trackId);

      if (!removeRes)
      {
        return std::unexpected{removeRes.error()};
      }

      if (!*removeRes)
      {
        return makeError(Error::Code::NotFound, std::format("track not found: {}", trackId));
      }

      work.changeSet = LibraryChangeSet{.tracksDeleted = {trackId},
                                        .listsUpserted = work.reply.removedFromListIds,
                                        .listOrderChanges = std::move(changedLists.orderChanges)};
      return work;
    }

    struct CreateTrackFacts final
    {
      TrackId provisionalTrackId{};
      std::string uri{};
      std::string title{};
      std::string artist{};
    };

    CreateTrackReply committedCreateTrackReply(CreateTrackFacts facts)
    {
      return CreateTrackReply{
        .trackId = facts.provisionalTrackId,
        .uri = std::move(facts.uri),
        .title = std::move(facts.title),
        .artist = std::move(facts.artist),
      };
    }

    PreviewCreateTrackReply previewCreateTrackReply(CreateTrackFacts facts)
    {
      return PreviewCreateTrackReply{
        .uri = std::move(facts.uri), .title = std::move(facts.title), .artist = std::move(facts.artist)};
    }

    Result<CreateTrackFacts> applyCreateTrackInTransaction(library::LibraryWrite& transaction,
                                                           ImportTarget const& target,
                                                           MediaTrack& mediaTrack)
    {
      auto writer = transaction.tracks();

      if (auto const optExistingManifest = writer.manifest(target.uri); optExistingManifest)
      {
        return makeError(Error::Code::Conflict, std::format("track file is already imported: {}", target.uri));
      }

      auto& builder = mediaTrack.builder();
      builder.property().uri(target.uri);
      auto const title = std::string{builder.metadata().title()};
      auto const artist = std::string{builder.metadata().artist()};

      auto fileEc = std::error_code{};
      auto const fileSize = std::filesystem::file_size(target.fullPath, fileEc);

      if (fileEc)
      {
        return makeError(
          Error::Code::IoError,
          std::format("failed to inspect track file '{}': {}", utility::pathToUtf8(target.fullPath), fileEc.message()));
      }

      auto const lastWriteTime = std::filesystem::last_write_time(target.fullPath, fileEc);

      if (fileEc)
      {
        return makeError(
          Error::Code::IoError,
          std::format(
            "failed to read track file timestamp '{}': {}", utility::pathToUtf8(target.fullPath), fileEc.message()));
      }

      auto manifestBuilder = library::FileManifestBuilder::makeEmpty();
      manifestBuilder.fileSize(static_cast<std::uint64_t>(fileSize))
        .mtime(static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(lastWriteTime.time_since_epoch()).count()));

      auto createRes = writer.create(builder, manifestBuilder);

      if (!createRes)
      {
        return storageError("Failed to create track data", createRes.error());
      }

      return CreateTrackFacts{
        .provisionalTrackId = *createRes,
        .uri = target.uri,
        .title = title,
        .artist = artist,
      };
    }
  } // namespace

  struct LibraryCommands::Impl final
  {
    async::Task<Result<UpdateTrackMetadataReply>> previewUpdateMetadata(LibraryWriteLane::Submission submission,
                                                                        std::vector<TrackId> trackIds,
                                                                        MetadataPatch patch);
    async::Task<Result<TrackAuthoringResult<UpdateTrackMetadataReply>>>
    applyUpdateMetadata(LibraryWriteLane::Submission submission, BoundTrackTargets targets, MetadataPatch patch);
    async::Task<Result<EditTrackTagsReply>> previewEditTags(LibraryWriteLane::Submission submission,
                                                            std::vector<TrackId> trackIds,
                                                            std::vector<std::string> tagsToAdd,
                                                            std::vector<std::string> tagsToRemove);
    async::Task<Result<TrackAuthoringResult<EditTrackTagsReply>>> applyEditTags(LibraryWriteLane::Submission submission,
                                                                                BoundTrackTargets targets,
                                                                                std::vector<std::string> tagsToAdd,
                                                                                std::vector<std::string> tagsToRemove);
    async::Task<Result<UpdateTrackPropertiesReply>> previewUpdateProperties(LibraryWriteLane::Submission submission,
                                                                            std::vector<TrackId> trackIds,
                                                                            TrackPropertiesPatch patch);
    async::Task<Result<TrackAuthoringResult<UpdateTrackPropertiesReply>>> applyUpdateProperties(
      LibraryWriteLane::Submission submission,
      BoundTrackTargets targets,
      TrackPropertiesPatch patch);
    async::Task<Result<AddTracksToListReply>> previewAddTracksToList(LibraryWriteLane::Submission submission,
                                                                     ListId listId,
                                                                     std::vector<TrackId> trackIds);
    async::Task<Result<TrackAuthoringResult<AddTracksToListReply>>>
    applyAddTracksToList(LibraryWriteLane::Submission submission, ListId listId, BoundTrackTargets targets);
    async::Task<Result<RemoveTracksFromListReply>> previewRemoveTracksFromList(LibraryWriteLane::Submission submission,
                                                                               ListId listId,
                                                                               std::vector<TrackId> trackIds);
    async::Task<Result<TrackAuthoringResult<RemoveTracksFromListReply>>>
    applyRemoveTracksFromList(LibraryWriteLane::Submission submission, ListId listId, BoundTrackTargets targets);
    async::Task<Result<ListId>> createList(LibraryWriteLane::Submission submission, ListDraft draft);
    async::Task<Result<>> previewCreateList(LibraryWriteLane::Submission submission, ListDraft draft);
    async::Task<Result<UpdateListReply>> updateList(LibraryWriteLane::Submission submission, ListDraft draft);
    async::Task<Result<UpdateListReply>> previewUpdateList(LibraryWriteLane::Submission submission, ListDraft draft);
    async::Task<Result<AuthoringResult<MoveListOrderReply>>> applyMoveListOrder(
      LibraryWriteLane::Submission submission,
      BoundListOrder order,
      std::vector<TrackId> selectedTrackIds,
      std::optional<TrackId> optBeforeTrackId);
    async::Task<Result<AuthoringResult<ResetListOrderReply>>> applyResetListOrder(
      LibraryWriteLane::Submission submission,
      BoundListOrder order);
    async::Task<Result<AuthoringResult<ForgetHiddenListOrderReply>>> applyForgetHiddenListOrder(
      LibraryWriteLane::Submission submission,
      BoundListOrder order);
    async::Task<Result<DeleteListReply>> deleteList(LibraryWriteLane::Submission submission,
                                                    ListId listId,
                                                    DeleteListOptions options);
    async::Task<Result<DeleteListReply>> previewDeleteList(LibraryWriteLane::Submission submission,
                                                           ListId listId,
                                                           DeleteListOptions options);
    async::Task<Result<DeleteListSubtreeReply>> deleteListAndDescendants(LibraryWriteLane::Submission submission,
                                                                         ListId listId,
                                                                         DeleteListOptions options);
    async::Task<Result<DeleteListSubtreeReply>> previewDeleteListAndDescendants(LibraryWriteLane::Submission submission,
                                                                                ListId listId,
                                                                                DeleteListOptions options);
    async::Task<Result<DeleteTrackReply>> deleteTrack(LibraryWriteLane::Submission submission, TrackId trackId);
    async::Task<Result<DeleteTrackReply>> previewDeleteTrack(LibraryWriteLane::Submission submission, TrackId trackId);
    async::Task<Result<CreateTrackReply>> createTrackFromFile(LibraryWriteLane::Submission submission,
                                                              std::filesystem::path path) const;
    async::Task<Result<PreviewCreateTrackReply>> previewCreateTrackFromFile(LibraryWriteLane::Submission submission,
                                                                            std::filesystem::path path) const;

    library::MusicLibrary& library;
    LibraryWriteLane& writeLane;
    async::Runtime& asyncRuntime;
  };

  LibraryCommands::LibraryCommands(library::MusicLibrary& library,
                                   LibraryWriteLane& writeLane,
                                   async::Runtime& asyncRuntime)
    : _implPtr{std::make_shared<Impl>(library, writeLane, asyncRuntime)}
  {
  }

  LibraryCommands::~LibraryCommands() = default;

  async::Task<Result<TrackAuthoringResult<UpdateTrackMetadataReply>>> LibraryCommands::updateMetadata(
    BoundTrackTargets targets,
    MetadataPatch patch)
  {
    return submitWriterOperation<Result<TrackAuthoringResult<UpdateTrackMetadataReply>>>(
      _implPtr, &Impl::applyUpdateMetadata, std::move(targets), std::move(patch));
  }

  async::Task<Result<UpdateTrackMetadataReply>> LibraryCommands::previewUpdateMetadata(std::vector<TrackId> trackIds,
                                                                                       MetadataPatch patch)
  {
    return submitWriterOperation<Result<UpdateTrackMetadataReply>>(
      _implPtr, &Impl::previewUpdateMetadata, std::move(trackIds), std::move(patch));
  }

  async::Task<Result<TrackAuthoringResult<EditTrackTagsReply>>> LibraryCommands::editTags(
    BoundTrackTargets targets,
    std::vector<std::string> tagsToAdd,
    std::vector<std::string> tagsToRemove)
  {
    return submitWriterOperation<Result<TrackAuthoringResult<EditTrackTagsReply>>>(
      _implPtr, &Impl::applyEditTags, std::move(targets), std::move(tagsToAdd), std::move(tagsToRemove));
  }

  async::Task<Result<EditTrackTagsReply>> LibraryCommands::previewEditTags(std::vector<TrackId> trackIds,
                                                                           std::vector<std::string> tagsToAdd,
                                                                           std::vector<std::string> tagsToRemove)
  {
    return submitWriterOperation<Result<EditTrackTagsReply>>(
      _implPtr, &Impl::previewEditTags, std::move(trackIds), std::move(tagsToAdd), std::move(tagsToRemove));
  }

  async::Task<Result<TrackAuthoringResult<UpdateTrackPropertiesReply>>> LibraryCommands::updateProperties(
    BoundTrackTargets targets,
    TrackPropertiesPatch patch)
  {
    return submitWriterOperation<Result<TrackAuthoringResult<UpdateTrackPropertiesReply>>>(
      _implPtr, &Impl::applyUpdateProperties, std::move(targets), std::move(patch));
  }

  async::Task<Result<UpdateTrackPropertiesReply>> LibraryCommands::previewUpdateProperties(
    std::vector<TrackId> trackIds,
    TrackPropertiesPatch patch)
  {
    return submitWriterOperation<Result<UpdateTrackPropertiesReply>>(
      _implPtr, &Impl::previewUpdateProperties, std::move(trackIds), std::move(patch));
  }

  async::Task<Result<TrackAuthoringResult<AddTracksToListReply>>> LibraryCommands::addTracksToList(
    ListId const listId,
    BoundTrackTargets targets)
  {
    return submitWriterOperation<Result<TrackAuthoringResult<AddTracksToListReply>>>(
      _implPtr, &Impl::applyAddTracksToList, listId, std::move(targets));
  }

  async::Task<Result<AddTracksToListReply>> LibraryCommands::previewAddTracksToList(ListId const listId,
                                                                                    std::vector<TrackId> trackIds)
  {
    return submitWriterOperation<Result<AddTracksToListReply>>(
      _implPtr, &Impl::previewAddTracksToList, listId, std::move(trackIds));
  }

  async::Task<Result<TrackAuthoringResult<RemoveTracksFromListReply>>> LibraryCommands::removeTracksFromList(
    ListId const listId,
    BoundTrackTargets targets)
  {
    return submitWriterOperation<Result<TrackAuthoringResult<RemoveTracksFromListReply>>>(
      _implPtr, &Impl::applyRemoveTracksFromList, listId, std::move(targets));
  }

  async::Task<Result<RemoveTracksFromListReply>> LibraryCommands::previewRemoveTracksFromList(
    ListId const listId,
    std::vector<TrackId> trackIds)
  {
    return submitWriterOperation<Result<RemoveTracksFromListReply>>(
      _implPtr, &Impl::previewRemoveTracksFromList, listId, std::move(trackIds));
  }

  async::Task<Result<ListId>> LibraryCommands::createList(ListDraft draft)
  {
    return submitWriterOperation<Result<ListId>>(_implPtr, &Impl::createList, std::move(draft));
  }

  async::Task<Result<>> LibraryCommands::previewCreateList(ListDraft draft)
  {
    return submitWriterOperation<Result<>>(_implPtr, &Impl::previewCreateList, std::move(draft));
  }

  async::Task<Result<UpdateListReply>> LibraryCommands::updateList(ListDraft draft)
  {
    return submitWriterOperation<Result<UpdateListReply>>(_implPtr, &Impl::updateList, std::move(draft));
  }

  async::Task<Result<UpdateListReply>> LibraryCommands::previewUpdateList(ListDraft draft)
  {
    return submitWriterOperation<Result<UpdateListReply>>(_implPtr, &Impl::previewUpdateList, std::move(draft));
  }

  async::Task<Result<AuthoringResult<MoveListOrderReply>>> LibraryCommands::moveListOrder(
    BoundListOrder order,
    std::vector<TrackId> selectedTrackIds,
    std::optional<TrackId> const optBeforeTrackId)
  {
    return submitWriterOperation<Result<AuthoringResult<MoveListOrderReply>>>(
      _implPtr, &Impl::applyMoveListOrder, std::move(order), std::move(selectedTrackIds), optBeforeTrackId);
  }

  async::Task<Result<AuthoringResult<ResetListOrderReply>>> LibraryCommands::resetListOrder(BoundListOrder order)
  {
    return submitWriterOperation<Result<AuthoringResult<ResetListOrderReply>>>(
      _implPtr, &Impl::applyResetListOrder, std::move(order));
  }

  async::Task<Result<AuthoringResult<ForgetHiddenListOrderReply>>> LibraryCommands::forgetHiddenListOrder(
    BoundListOrder order)
  {
    return submitWriterOperation<Result<AuthoringResult<ForgetHiddenListOrderReply>>>(
      _implPtr, &Impl::applyForgetHiddenListOrder, std::move(order));
  }

  async::Task<Result<DeleteListReply>> LibraryCommands::deleteList(ListId const listId, DeleteListOptions const options)
  {
    return submitWriterOperation<Result<DeleteListReply>>(_implPtr, &Impl::deleteList, listId, options);
  }

  async::Task<Result<DeleteListReply>> LibraryCommands::previewDeleteList(ListId const listId,
                                                                          DeleteListOptions const options)
  {
    return submitWriterOperation<Result<DeleteListReply>>(_implPtr, &Impl::previewDeleteList, listId, options);
  }

  async::Task<Result<DeleteListSubtreeReply>> LibraryCommands::deleteListAndDescendants(ListId const listId,
                                                                                        DeleteListOptions const options)
  {
    return submitWriterOperation<Result<DeleteListSubtreeReply>>(
      _implPtr, &Impl::deleteListAndDescendants, listId, options);
  }

  async::Task<Result<DeleteListSubtreeReply>> LibraryCommands::previewDeleteListAndDescendants(
    ListId const listId,
    DeleteListOptions const options)
  {
    return submitWriterOperation<Result<DeleteListSubtreeReply>>(
      _implPtr, &Impl::previewDeleteListAndDescendants, listId, options);
  }

  async::Task<Result<DeleteTrackReply>> LibraryCommands::deleteTrack(TrackId const trackId)
  {
    return submitWriterOperation<Result<DeleteTrackReply>>(_implPtr, &Impl::deleteTrack, trackId);
  }

  async::Task<Result<DeleteTrackReply>> LibraryCommands::previewDeleteTrack(TrackId const trackId)
  {
    return submitWriterOperation<Result<DeleteTrackReply>>(_implPtr, &Impl::previewDeleteTrack, trackId);
  }

  async::Task<Result<CreateTrackReply>> LibraryCommands::createTrackFromFile(std::filesystem::path path)
  {
    return submitWriterOperation<Result<CreateTrackReply>>(_implPtr, &Impl::createTrackFromFile, std::move(path));
  }

  async::Task<Result<PreviewCreateTrackReply>> LibraryCommands::previewCreateTrackFromFile(std::filesystem::path path)
  {
    return submitWriterOperation<Result<PreviewCreateTrackReply>>(
      _implPtr, &Impl::previewCreateTrackFromFile, std::move(path));
  }

  async::Task<Result<UpdateTrackMetadataReply>> LibraryCommands::Impl::previewUpdateMetadata(
    LibraryWriteLane::Submission submission,
    std::vector<TrackId> trackIds,
    MetadataPatch patch)
  {
    return applyInteractivePreviewAsync(
      std::move(submission),
      [this, trackIds = std::move(trackIds), patch = std::move(patch)](library::LibraryWrite& transaction)
      { return applyMetadataPatchInTransaction(library, transaction, trackIds, patch); });
  }

  async::Task<Result<TrackAuthoringResult<UpdateTrackMetadataReply>>> LibraryCommands::Impl::applyUpdateMetadata(
    LibraryWriteLane::Submission submission,
    BoundTrackTargets targets,
    MetadataPatch patch)
  {
    return executeBoundTrackAuthoringAsync<UpdateTrackMetadataReply>(
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
    return applyInteractivePreviewAsync(
      std::move(submission),
      [this, trackIds = std::move(trackIds), tagsToAdd = std::move(tagsToAdd), tagsToRemove = std::move(tagsToRemove)](
        library::LibraryWrite& transaction)
      { return applyTagPatchInTransaction(library, transaction, trackIds, tagsToAdd, tagsToRemove); });
  }

  async::Task<Result<TrackAuthoringResult<EditTrackTagsReply>>> LibraryCommands::Impl::applyEditTags(
    LibraryWriteLane::Submission submission,
    BoundTrackTargets targets,
    std::vector<std::string> tagsToAdd,
    std::vector<std::string> tagsToRemove)
  {
    return executeBoundTrackAuthoringAsync<EditTrackTagsReply>(
      std::move(submission),
      std::move(targets),
      "Edit track tags",
      [this, tagsToAdd = std::move(tagsToAdd), tagsToRemove = std::move(tagsToRemove)](
        library::LibraryWrite& transaction,
        std::span<TrackId const> trackIds) -> Result<OperationOutcome<EditTrackTagsReply>>
      {
        auto replyRes = applyTagPatchInTransaction(library, transaction, trackIds, tagsToAdd, tagsToRemove);

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

  async::Task<Result<UpdateTrackPropertiesReply>> LibraryCommands::Impl::previewUpdateProperties(
    LibraryWriteLane::Submission submission,
    std::vector<TrackId> trackIds,
    TrackPropertiesPatch patch)
  {
    return applyInteractivePreviewAsync(
      std::move(submission),
      [this, trackIds = std::move(trackIds), patch = std::move(patch)](library::LibraryWrite& transaction)
      { return applyPropertiesPatchInTransaction(library, transaction, trackIds, patch); });
  }

  async::Task<Result<TrackAuthoringResult<UpdateTrackPropertiesReply>>> LibraryCommands::Impl::applyUpdateProperties(
    LibraryWriteLane::Submission submission,
    BoundTrackTargets targets,
    TrackPropertiesPatch patch)
  {
    return executeBoundTrackAuthoringAsync<UpdateTrackPropertiesReply>(
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

  async::Task<Result<AddTracksToListReply>> LibraryCommands::Impl::previewAddTracksToList(
    LibraryWriteLane::Submission submission,
    ListId const listId,
    std::vector<TrackId> trackIds)
  {
    return applyInteractivePreviewAsync(
      std::move(submission),
      [this, listId, trackIds = std::move(trackIds)](library::LibraryWrite& transaction)
      { return applyAddTracksToListInTransaction(library, transaction, listId, trackIds); });
  }

  async::Task<Result<TrackAuthoringResult<AddTracksToListReply>>> LibraryCommands::Impl::applyAddTracksToList(
    LibraryWriteLane::Submission submission,
    ListId const listId,
    BoundTrackTargets targets)
  {
    return executeBoundTrackAuthoringAsync<AddTracksToListReply>(
      std::move(submission),
      std::move(targets),
      "Add tracks to list",
      [this, listId](library::LibraryWrite& transaction,
                     std::span<TrackId const> trackIds) -> Result<OperationOutcome<AddTracksToListReply>>
      {
        auto replyRes = applyAddTracksToListInTransaction(library, transaction, listId, trackIds);

        if (!replyRes)
        {
          return std::unexpected{replyRes.error()};
        }

        auto reply = std::move(*replyRes);

        if (reply.tagEdit.changes.empty())
        {
          return Unchanged<AddTracksToListReply>{.value = std::move(reply)};
        }

        auto mutatedIds =
          reply.tagEdit.changes | std::views::transform(&TrackTagsChange::trackId) | std::ranges::to<std::vector>();
        return Changed<AddTracksToListReply>{
          .value = std::move(reply),
          .changeSet = LibraryChangeSet{.tracksMutated = std::move(mutatedIds)},
        };
      });
  }

  async::Task<Result<RemoveTracksFromListReply>> LibraryCommands::Impl::previewRemoveTracksFromList(
    LibraryWriteLane::Submission submission,
    ListId const listId,
    std::vector<TrackId> trackIds)
  {
    auto workRes = co_await applyInteractivePreviewAsync(
      std::move(submission),
      [this, listId, trackIds = std::move(trackIds)](library::LibraryWrite& transaction)
      { return applyRemoveTracksFromListInTransaction(library, transaction, listId, trackIds); });

    if (!workRes)
    {
      co_return std::unexpected{workRes.error()};
    }

    co_return std::move(workRes->reply);
  }

  async::Task<Result<TrackAuthoringResult<RemoveTracksFromListReply>>> LibraryCommands::Impl::applyRemoveTracksFromList(
    LibraryWriteLane::Submission submission,
    ListId const listId,
    BoundTrackTargets targets)
  {
    return executeBoundTrackAuthoringAsync<RemoveTracksFromListReply>(
      std::move(submission),
      std::move(targets),
      "Remove tracks from list",
      [this, listId](library::LibraryWrite& transaction,
                     std::span<TrackId const> trackIds) -> Result<OperationOutcome<RemoveTracksFromListReply>>
      {
        auto workRes = applyRemoveTracksFromListInTransaction(library, transaction, listId, trackIds);

        if (!workRes)
        {
          return std::unexpected{workRes.error()};
        }

        auto work = std::move(*workRes);
        auto mutatedIds = work.reply.tagEdit.changes | std::views::transform(&TrackTagsChange::trackId) |
                          std::ranges::to<std::vector>();

        if (auto const orderChanged = work.optOrderChange.has_value(); mutatedIds.empty() && !orderChanged)
        {
          return Unchanged<RemoveTracksFromListReply>{.value = std::move(work.reply)};
        }

        auto changeSet = LibraryChangeSet{.tracksMutated = std::move(mutatedIds)};

        if (work.optOrderChange)
        {
          changeSet.listsUpserted.push_back(listId);
          changeSet.listOrderChanges.push_back(std::move(*work.optOrderChange));
        }

        return Changed<RemoveTracksFromListReply>{
          .value = std::move(work.reply),
          .changeSet = std::move(changeSet),
        };
      });
  }

  async::Task<Result<ListId>> LibraryCommands::Impl::createList(LibraryWriteLane::Submission submission,
                                                                ListDraft draft)
  {
    auto executionRes = co_await executeInteractiveMutationAsync(
      std::move(submission),
      "Create list",
      [draft = std::move(draft)](library::LibraryWrite& transaction) -> Result<OperationOutcome<ListId>>
      {
        auto listIdRes = createListInTransaction(transaction, draft);

        if (!listIdRes)
        {
          return std::unexpected{listIdRes.error()};
        }

        auto const listId = *listIdRes;
        return Changed<ListId>{
          .value = listId,
          .changeSet = LibraryChangeSet{.listsUpserted = {listId}},
        };
      });

    if (!executionRes)
    {
      co_return std::unexpected{executionRes.error()};
    }

    AO_INVARIANT(executionRes->optCommittedRevision, "List creation did not commit its generated identity");
    co_return executionRes->value;
  }

  async::Task<Result<>> LibraryCommands::Impl::previewCreateList(LibraryWriteLane::Submission submission,
                                                                 ListDraft draft)
  {
    auto listIdRes =
      co_await applyInteractivePreviewAsync(std::move(submission),
                                            [draft = std::move(draft)](library::LibraryWrite& transaction)
                                            { return createListInTransaction(transaction, draft); });

    if (!listIdRes)
    {
      co_return std::unexpected{listIdRes.error()};
    }

    co_return Result<>{};
  }

  async::Task<Result<UpdateListReply>> LibraryCommands::Impl::updateList(LibraryWriteLane::Submission submission,
                                                                         ListDraft draft)
  {
    auto executionRes = co_await executeInteractiveMutationAsync(
      std::move(submission),
      "Update list",
      [draft = std::move(draft)](library::LibraryWrite& transaction) -> Result<OperationOutcome<UpdateListReply>>
      {
        auto replyRes = updateListInTransaction(transaction, draft);

        if (!replyRes)
        {
          return std::unexpected{replyRes.error()};
        }

        auto reply = std::move(*replyRes);

        if (!reply.changed)
        {
          return Unchanged<UpdateListReply>{.value = std::move(reply)};
        }

        return Changed<UpdateListReply>{
          .value = std::move(reply),
          .changeSet = LibraryChangeSet{.listsUpserted = {draft.listId}},
        };
      });

    if (!executionRes)
    {
      co_return std::unexpected{executionRes.error()};
    }

    co_return std::move(executionRes->value);
  }

  async::Task<Result<UpdateListReply>> LibraryCommands::Impl::previewUpdateList(LibraryWriteLane::Submission submission,
                                                                                ListDraft draft)
  {
    return applyInteractivePreviewAsync(std::move(submission),
                                        [draft = std::move(draft)](library::LibraryWrite& transaction)
                                        { return updateListInTransaction(transaction, draft); });
  }

  async::Task<Result<AuthoringResult<MoveListOrderReply>>> LibraryCommands::Impl::applyMoveListOrder(
    LibraryWriteLane::Submission submission,
    BoundListOrder order,
    std::vector<TrackId> selectedTrackIds,
    std::optional<TrackId> const optBeforeTrackId)
  {
    auto start = co_await LibraryWriteLane::beginListOrderAuthoringMutationAsync(std::move(submission), order);
    auto result = AuthoringResult<MoveListOrderReply>{.status = start.status};

    if (!start.optMutation)
    {
      co_return result;
    }

    auto const effectiveTrackIds = order.effectiveTrackIds();
    auto const effectiveMembership = std::unordered_set<TrackId>{effectiveTrackIds.begin(), effectiveTrackIds.end()};
    auto selectedMembership = std::unordered_set<TrackId>{};

    for (auto const trackId : selectedTrackIds)
    {
      if (!effectiveMembership.contains(trackId))
      {
        co_return makeError(
          Error::Code::InvalidInput, std::format("List order selection is not in the bound source: {}", trackId));
      }

      selectedMembership.insert(trackId);
    }

    for (auto const trackId : effectiveTrackIds)
    {
      if (selectedMembership.contains(trackId))
      {
        result.reply.selectedTrackIds.push_back(trackId);
      }
    }

    result.reply.optBeforeTrackId = optBeforeTrackId;

    if (result.reply.selectedTrackIds.empty())
    {
      result.status = AuthoringStatus::NoOp;
      co_return result;
    }

    if (optBeforeTrackId &&
        (!effectiveMembership.contains(*optBeforeTrackId) || selectedMembership.contains(*optBeforeTrackId)))
    {
      co_return makeError(Error::Code::InvalidInput, "List order anchor must be an unselected bound source track");
    }

    auto desiredEffectiveTrackIds = std::vector<TrackId>{};
    desiredEffectiveTrackIds.reserve(effectiveTrackIds.size());

    for (auto const trackId : effectiveTrackIds)
    {
      if (!selectedMembership.contains(trackId))
      {
        desiredEffectiveTrackIds.push_back(trackId);
      }
    }

    auto insertion = desiredEffectiveTrackIds.end();

    if (optBeforeTrackId)
    {
      insertion = std::ranges::find(desiredEffectiveTrackIds, *optBeforeTrackId);
    }

    desiredEffectiveTrackIds.insert(
      insertion, result.reply.selectedTrackIds.begin(), result.reply.selectedTrackIds.end());

    if (std::ranges::equal(effectiveTrackIds, desiredEffectiveTrackIds))
    {
      result.status = AuthoringStatus::NoOp;
      co_return result;
    }

    auto executionRes = co_await start.optMutation->executeAsync(
      [&order, &desiredEffectiveTrackIds, &result, optBeforeTrackId](
        library::LibraryWrite& transaction) -> Result<OperationOutcome<MoveListOrderReply>>
      {
        auto changeSetRes = applyMoveListOrderInTransaction(
          transaction, order, result.reply.selectedTrackIds, desiredEffectiveTrackIds, optBeforeTrackId);

        if (!changeSetRes)
        {
          return std::unexpected{changeSetRes.error()};
        }

        return Changed<MoveListOrderReply>{
          .value = std::move(result.reply),
          .changeSet = std::move(*changeSetRes),
        };
      },
      "Move list order");

    if (!executionRes)
    {
      co_return std::unexpected{executionRes.error()};
    }

    result.reply = std::move(executionRes->value);
    result.status = AuthoringStatus::Applied;
    co_return result;
  }

  async::Task<Result<AuthoringResult<ResetListOrderReply>>> LibraryCommands::Impl::applyResetListOrder(
    LibraryWriteLane::Submission submission,
    BoundListOrder order)
  {
    return executeBoundListOrderAuthoringAsync<ResetListOrderReply>(
      std::move(submission),
      std::move(order),
      "Reset list order",
      [](library::LibraryWrite& transaction,
         BoundListOrder const& order) -> Result<OperationOutcome<ResetListOrderReply>>
      {
        auto listWriter = transaction.lists();
        auto viewRes = requireList(listWriter, order.listId());

        if (!viewRes)
        {
          return std::unexpected{viewRes.error()};
        }

        auto const oldOrderTrackIds = orderTrackIdsFrom(*viewRes);
        auto reply = ResetListOrderReply{.forgottenPositionCount = oldOrderTrackIds.size()};

        if (oldOrderTrackIds.empty())
        {
          return Unchanged<ResetListOrderReply>{.value = std::move(reply)};
        }

        auto list = listWithOrder(*viewRes, {});

        if (auto updateRes = listWriter.update(order.listId(), list); !updateRes)
        {
          return storageError("Failed to reset List order", updateRes.error());
        }

        return Changed<ResetListOrderReply>{
          .value = std::move(reply),
          .changeSet = LibraryChangeSet{.listsUpserted = {order.listId()},
                                        .listOrderChanges =
                                          {
                                            ListOrderChange{.listId = order.listId(), .operation = ListOrderReset{}},
                                          }},
        };
      });
  }

  async::Task<Result<AuthoringResult<ForgetHiddenListOrderReply>>> LibraryCommands::Impl::applyForgetHiddenListOrder(
    LibraryWriteLane::Submission submission,
    BoundListOrder order)
  {
    return executeBoundListOrderAuthoringAsync<ForgetHiddenListOrderReply>(
      std::move(submission),
      std::move(order),
      "Forget hidden list order",
      [](library::LibraryWrite& transaction,
         BoundListOrder const& order) -> Result<OperationOutcome<ForgetHiddenListOrderReply>>
      {
        auto listWriter = transaction.lists();
        auto viewRes = requireList(listWriter, order.listId());

        if (!viewRes)
        {
          return std::unexpected{viewRes.error()};
        }

        auto const oldOrderTrackIds = orderTrackIdsFrom(*viewRes);
        auto const effectiveTrackIds = order.effectiveTrackIds();
        auto const effectiveMembership =
          std::unordered_set<TrackId>{effectiveTrackIds.begin(), effectiveTrackIds.end()};
        auto nextOrderTrackIds = std::vector<TrackId>{};
        nextOrderTrackIds.reserve(oldOrderTrackIds.size());

        for (auto const trackId : oldOrderTrackIds)
        {
          if (effectiveMembership.contains(trackId))
          {
            nextOrderTrackIds.push_back(trackId);
          }
        }

        auto reply =
          ForgetHiddenListOrderReply{.forgottenPositionCount = oldOrderTrackIds.size() - nextOrderTrackIds.size()};

        if (reply.forgottenPositionCount == 0)
        {
          return Unchanged<ForgetHiddenListOrderReply>{.value = std::move(reply)};
        }

        auto list = listWithOrder(*viewRes, nextOrderTrackIds);

        if (auto updateRes = listWriter.update(order.listId(), list); !updateRes)
        {
          return storageError("Failed to forget hidden List positions", updateRes.error());
        }

        auto script = delta::diff(oldOrderTrackIds, nextOrderTrackIds);
        return Changed<ForgetHiddenListOrderReply>{
          .value = std::move(reply),
          .changeSet = LibraryChangeSet{.listsUpserted = {order.listId()},
                                        .listOrderChanges =
                                          {
                                            ListOrderChange{.listId = order.listId(), .operation = std::move(script)},
                                          }},
        };
      });
  }

  async::Task<Result<DeleteListReply>> LibraryCommands::Impl::deleteList(LibraryWriteLane::Submission submission,
                                                                         ListId const listId,
                                                                         DeleteListOptions const options)
  {
    return executeChangedWorkAsync<DeleteListReply>(
      std::move(submission),
      "Delete list",
      [this, listId, options](library::LibraryWrite& transaction)
      { return applyDeleteListInTransaction(library, transaction, listId, options); });
  }

  async::Task<Result<DeleteListReply>> LibraryCommands::Impl::previewDeleteList(LibraryWriteLane::Submission submission,
                                                                                ListId const listId,
                                                                                DeleteListOptions const options)
  {
    return previewChangedWorkAsync<DeleteListReply>(
      std::move(submission),
      [this, listId, options](library::LibraryWrite& transaction)
      { return applyDeleteListInTransaction(library, transaction, listId, options); });
  }

  async::Task<Result<DeleteListSubtreeReply>> LibraryCommands::Impl::deleteListAndDescendants(
    LibraryWriteLane::Submission submission,
    ListId const listId,
    DeleteListOptions const options)
  {
    return executeChangedWorkAsync<DeleteListSubtreeReply>(
      std::move(submission),
      "Delete list subtree",
      [this, listId, options](library::LibraryWrite& transaction)
      { return applyDeleteListSubtreeInTransaction(library, transaction, listId, options); });
  }

  async::Task<Result<DeleteListSubtreeReply>> LibraryCommands::Impl::previewDeleteListAndDescendants(
    LibraryWriteLane::Submission submission,
    ListId const listId,
    DeleteListOptions const options)
  {
    return previewChangedWorkAsync<DeleteListSubtreeReply>(
      std::move(submission),
      [this, listId, options](library::LibraryWrite& transaction)
      { return applyDeleteListSubtreeInTransaction(library, transaction, listId, options); });
  }

  async::Task<Result<DeleteTrackReply>> LibraryCommands::Impl::deleteTrack(LibraryWriteLane::Submission submission,
                                                                           TrackId const trackId)
  {
    return executeChangedWorkAsync<DeleteTrackReply>(
      std::move(submission),
      "Delete track",
      [this, trackId](library::LibraryWrite& transaction)
      { return applyDeleteTrackInTransaction(library, transaction, trackId); });
  }

  async::Task<Result<DeleteTrackReply>> LibraryCommands::Impl::previewDeleteTrack(
    LibraryWriteLane::Submission submission,
    TrackId const trackId)
  {
    return previewChangedWorkAsync<DeleteTrackReply>(
      std::move(submission),
      [this, trackId](library::LibraryWrite& transaction)
      { return applyDeleteTrackInTransaction(library, transaction, trackId); });
  }

  async::Task<Result<CreateTrackReply>> LibraryCommands::Impl::createTrackFromFile(
    LibraryWriteLane::Submission submission,
    std::filesystem::path path) const
  {
    auto preparedRes = prepareTrackImport(library, path);

    if (!preparedRes)
    {
      co_return std::unexpected{preparedRes.error()};
    }

    auto& prepared = *preparedRes;

    auto executionRes = co_await executeInteractiveMutationAsync(
      std::move(submission),
      "Create track",
      [&prepared](library::LibraryWrite& transaction) -> Result<OperationOutcome<CreateTrackFacts>>
      {
        auto factsRes = applyCreateTrackInTransaction(transaction, prepared.target, prepared.mediaTrack);

        if (!factsRes)
        {
          return std::unexpected{factsRes.error()};
        }

        auto facts = std::move(*factsRes);
        auto const trackId = facts.provisionalTrackId;
        return Changed<CreateTrackFacts>{
          .value = std::move(facts),
          .changeSet = LibraryChangeSet{.tracksInserted = {trackId}},
        };
      });

    if (!executionRes)
    {
      co_return std::unexpected{executionRes.error()};
    }

    AO_INVARIANT(executionRes->optCommittedRevision, "Track creation did not commit its generated identity");
    co_return committedCreateTrackReply(std::move(executionRes->value));
  }

  async::Task<Result<PreviewCreateTrackReply>> LibraryCommands::Impl::previewCreateTrackFromFile(
    LibraryWriteLane::Submission submission,
    std::filesystem::path path) const
  {
    auto preparedRes = prepareTrackImport(library, path);

    if (!preparedRes)
    {
      co_return std::unexpected{preparedRes.error()};
    }

    auto& prepared = *preparedRes;

    auto factsRes = co_await applyInteractivePreviewAsync(
      std::move(submission),
      [&prepared](library::LibraryWrite& transaction)
      { return applyCreateTrackInTransaction(transaction, prepared.target, prepared.mediaTrack); });

    if (!factsRes)
    {
      co_return std::unexpected{factsRes.error()};
    }

    co_return previewCreateTrackReply(std::move(*factsRes));
  }
} // namespace ao::rt
