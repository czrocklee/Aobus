// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors
//
// Review evidence only: no machine-dependent pass/fail thresholds.

#include "PerformanceReport.h"
#include "lib/audio/NullBackend.h"
#include "lib/query/detail/Normalize.h"
#include "runtime/library/LibraryWriteLane.h"
#include "runtime/projection/StringArena.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include "test/unit/runtime/AsyncTestSupport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include "test/unit/runtime/library/LibraryWriteLaneTestSupport.h"
#include <ao/Error.h>
#include <ao/async/LoopExecutor.h>
#include <ao/audio/Backend.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/BackendProvider.h>
#include <ao/audio/Device.h>
#include <ao/audio/Engine.h>
#include <ao/audio/Player.h>
#include <ao/audio/RouteAnchor.h>
#include <ao/audio/flow/Graph.h>
#include <ao/i18n/IcuCompletionAliases.h>
#include <ao/i18n/IcuTextOrdering.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/query/FormatExpression.h>
#include <ao/query/Parser.h>
#include <ao/query/QueryCompilation.h>
#include <ao/query/Serializer.h>
#include <ao/rt/Log.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/completion/CompletionAliasPolicy.h>
#include <ao/rt/completion/CompletionResult.h>
#include <ao/rt/completion/CompletionService.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryPaths.h>
#include <ao/rt/ordering/TextOrderingPolicy.h>
#include <ao/uimodel/library/track/TrackFilter.h>
#include <ao/utility/Path.h>
#include <ao/utility/ScopedRegistration.h>
#include <ao/utility/String.h>
#include <ao/utility/UnicodeText.h>

#include <boost/unordered/unordered_flat_map.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    // One producer is joined before subscriptions or the fixture are retired.
    class ObservationProvider final : public audio::BackendProvider
    {
    public:
      void shutdown() noexcept override
      {
        _onDevices = {};
        _onGraph = {};
      }
      Status status() const override { return {.descriptor = {.id = audio::kBackendNone}}; }
      utility::ScopedRegistration subscribeDevices(OnDevicesChangedCallback callback) override
      {
        _onDevices = std::move(callback);
        _onDevices({device()});
        return {};
      }
      utility::ScopedRegistration subscribeGraph(std::string_view /*anchorId*/,
                                                 OnGraphChangedCallback callback) override
      {
        _onGraph = std::move(callback);
        return {};
      }
      std::unique_ptr<audio::Backend> createBackend(audio::Device const& /*device*/,
                                                    audio::ProfileId const& /*profile*/) override
      {
        return std::make_unique<audio::NullBackend>();
      }
      static audio::Device device()
      {
        return {.id = audio::DeviceId{"audit"}, .displayName = "Audit", .backendId = audio::kBackendNone};
      }
      void emitGraph(audio::flow::Graph const& graph) const { _onGraph(graph); }

    private:
      OnDevicesChangedCallback _onDevices;
      OnGraphChangedCallback _onGraph;
    };

    constexpr std::size_t kDefaultSamples = 20;
    constexpr std::size_t kDefaultWarmups = 1;
    constexpr std::size_t kRealLibraryTrackCount = 50000;
    constexpr std::size_t kCompletionAsciiTrackCount = 50000;
    constexpr std::size_t kCompletionCjkTrackCount = 5155;
    constexpr std::size_t kCompletionHanValueCount = 1619;
    constexpr std::size_t kCompletionKanaValueCount = 225;
    constexpr std::size_t kCompletionLookupLimit = 8;
    constexpr auto kCompletionFields = std::to_array({
      TrackField::Title,
      TrackField::Artist,
      TrackField::Album,
      TrackField::AlbumArtist,
      TrackField::Genre,
      TrackField::Composer,
      TrackField::Work,
    });

    struct Dataset final
    {
      std::string name;
      std::vector<std::string> titles;
      std::vector<std::string> dictionaryValues;
      std::vector<std::size_t> dictionaryIndices;
    };

    struct OrderEntry final
    {
      std::string_view groupOrderKey;
      std::string_view groupIdentityKey;
      std::string_view titleKey;
      std::size_t sourceIndex = 0;
    };

    struct PreparedOrder final
    {
      std::unique_ptr<rt::detail::StringArena> arenaPtr = std::make_unique<rt::detail::StringArena>();
      std::vector<OrderEntry> entries;
      std::size_t generatedKeyBytes = 0;
    };

    struct CachedDictionaryText final
    {
      std::string_view identityKey;
      std::string_view sortKey;
    };

    struct OperationResult final
    {
      std::uint64_t checksum = 0;
      std::size_t generatedKeyBytes = 0;
    };

    struct CompletionReviewHarness final
    {
      CompletionReviewHarness(MusicLibraryFixture& libraryFixture, CompletionAliasPolicy const* aliasPolicy)
        : asyncRuntime{executor}
        , changes{makeLibraryChanges(executor, libraryFixture.library())}
        , service{libraryFixture.library(), changes, nullptr, aliasPolicy}
        , writeLane{asyncRuntime.callbackExecutor(),
                    library::test::requireWritableLibrary(libraryFixture.library()),
                    changes}
        , completer{service}
      {
      }

      async::LoopExecutor executor;
      async::Runtime asyncRuntime;
      LibraryChanges changes;
      CompletionService service;
      LibraryWriteLane writeLane;
      uimodel::TrackFilterCompleter completer;
    };

    bool startsWithAsciiCaseInsensitive(std::string_view const text, std::string_view const prefix)
    {
      if (text.size() < prefix.size())
      {
        return false;
      }

      for (std::size_t index = 0; index < prefix.size(); ++index)
      {
        if (utility::toAsciiLower(text[index]) != utility::toAsciiLower(prefix[index]))
        {
          return false;
        }
      }

      return true;
    }

    std::string_view stripLeadingArticle(std::string_view text)
    {
      if (startsWithAsciiCaseInsensitive(text, "the "))
      {
        text.remove_prefix(4);
      }
      else if (startsWithAsciiCaseInsensitive(text, "an "))
      {
        text.remove_prefix(3);
      }
      else if (startsWithAsciiCaseInsensitive(text, "a "))
      {
        text.remove_prefix(2);
      }

      return text;
    }

    void foldAsciiInto(std::string& output, std::string_view const text)
    {
      output.clear();
      output.reserve(text.size());

      for (auto const ch : text)
      {
        output.push_back(utility::toAsciiLower(ch));
      }
    }

    bool isAsciiText(std::string_view const text)
    {
      return std::ranges::all_of(
        text, [](char const ch) { return static_cast<unsigned char>(ch) <= static_cast<unsigned char>(0x7f); });
    }

    void requireSortKeyInto(TextOrderingPolicy const& policy, std::string& output, std::string_view const text)
    {
      if (auto resultRes = policy.makeSortKeyInto(output, text); !resultRes)
      {
        throw std::runtime_error{resultRes.error().message};
      }
    }

    void deriveSortKeyInto(std::string& output, std::string_view const text, TextOrderingPolicy const* const optPolicy)
    {
      auto const orderingText = stripLeadingArticle(text);

      if (optPolicy == nullptr)
      {
        foldAsciiInto(output, orderingText);
        return;
      }

      requireSortKeyInto(*optPolicy, output, orderingText);
    }

    std::string_view internCounted(rt::detail::StringArena& arena,
                                   std::string_view const value,
                                   std::size_t& generatedKeyBytes)
    {
      auto const sizeBefore = arena.size();
      auto const interned = arena.intern(value);

      if (arena.size() != sizeBefore)
      {
        generatedKeyBytes += interned.size();
      }

      return interned;
    }

    std::string_view deriveIdentityKey(rt::detail::StringArena& arena,
                                       std::string& scratch,
                                       std::string_view const text,
                                       TextOrderingPolicy const* const optPolicy,
                                       std::size_t& generatedKeyBytes)
    {
      if (optPolicy == nullptr || isAsciiText(text))
      {
        foldAsciiInto(scratch, text);
        return internCounted(arena, scratch, generatedKeyBytes);
      }

      auto keyRes = utility::makeUtf8CaselessKey(text);

      if (!keyRes)
      {
        throw std::runtime_error{keyRes.error().message};
      }

      return internCounted(arena, *keyRes, generatedKeyBytes);
    }

    Dataset makeDataset(std::string_view const name, std::size_t const trackCount)
    {
      constexpr auto kLatinTitles = std::to_array<std::string_view>({
        "Die Ärzte",
        "Dvořák",
        "Björk",
        "Café Tacvba",
        "Straße",
        "Sigur Rós",
      });
      constexpr auto kCjkTitles = std::to_array<std::string_view>({
        "宇多田ヒカル",
        "坂本龍一",
        "誰か、海を。",
        "メタル",
        "めたる",
        "音楽図鑑",
      });

      auto dataset = Dataset{
        .name = std::string{name},
        .titles = {},
        .dictionaryValues = {},
        .dictionaryIndices = {},
      };
      auto const dictionaryCount = std::max<std::size_t>(1, trackCount / 50);
      dataset.titles.reserve(trackCount);
      dataset.dictionaryValues.reserve(dictionaryCount);
      dataset.dictionaryIndices.reserve(trackCount);

      auto stem = [&](std::size_t const index) -> std::string_view
      {
        if (name == "latin")
        {
          return kLatinTitles[index % kLatinTitles.size()];
        }

        if (name == "cjk")
        {
          return kCjkTitles[index % kCjkTitles.size()];
        }

        return index % 4 == 0 ? "The Track" : "Track";
      };

      for (std::size_t index = 0; index < dictionaryCount; ++index)
      {
        dataset.dictionaryValues.push_back(std::format("{} Artist {:04}", stem(index), index));
      }

      for (std::size_t index = 0; index < trackCount; ++index)
      {
        dataset.titles.push_back(std::format("{} {:06}", stem(index), index));
        dataset.dictionaryIndices.push_back(index % dictionaryCount);
      }

      return dataset;
    }

    std::optional<Dataset> makeRealLibraryDataset()
    {
      auto const* const rawRoot = std::getenv("AOBUS_PERF_LIBRARY_ROOT");

      if (rawRoot == nullptr || rawRoot[0] == '\0')
      {
        return std::nullopt;
      }

      auto const root = utility::pathFromUtf8(rawRoot);
      auto const paths = LibraryPaths{root};

      if (!paths.hasExistingDatabase())
      {
        throw std::runtime_error{std::format("performance library root '{}' has no existing Aobus database", rawRoot)};
      }

      auto libraryRes = library::MusicLibrary::open(root, paths.databasePath());

      if (!libraryRes)
      {
        throw std::runtime_error{std::format("could not open performance library: {}", libraryRes.error().message)};
      }

      auto dataset = Dataset{
        .name = "library-real",
        .titles = {},
        .dictionaryValues = {},
        .dictionaryIndices = {},
      };
      auto sourceTitles = std::vector<std::string>{};
      auto sourceDictionaryIndices = std::vector<std::size_t>{};
      auto dictionaryIndexById = std::unordered_map<std::uint32_t, std::size_t>{};

      auto const transaction = libraryRes->readTransaction();
      auto const reader = libraryRes->tracks().reader(transaction);
      auto const& dictionary = libraryRes->dictionary();
      sourceTitles.reserve(reader.entryCount());
      sourceDictionaryIndices.reserve(reader.entryCount());
      dictionaryIndexById.reserve(dictionary.size());

      for (auto const& [trackId, view] : reader)
      {
        std::ignore = trackId;
        auto const artistId = view.metadata().artistId();
        auto const [iterator, inserted] =
          dictionaryIndexById.try_emplace(artistId.raw(), dataset.dictionaryValues.size());

        if (inserted)
        {
          dataset.dictionaryValues.emplace_back(dictionary.getOrDefault(artistId));
        }

        sourceTitles.emplace_back(view.metadata().title());
        sourceDictionaryIndices.push_back(iterator->second);
      }

      if (sourceTitles.empty())
      {
        throw std::runtime_error{"performance library contains no tracks"};
      }

      dataset.titles.reserve(kRealLibraryTrackCount);
      dataset.dictionaryIndices.reserve(kRealLibraryTrackCount);

      for (std::size_t index = 0; index < kRealLibraryTrackCount; ++index)
      {
        auto const sourceIndex = index % sourceTitles.size();
        dataset.titles.push_back(sourceTitles[sourceIndex]);
        dataset.dictionaryIndices.push_back(sourceDictionaryIndices[sourceIndex]);
      }

      return dataset;
    }

    bool lessEntry(OrderEntry const& left, OrderEntry const& right)
    {
      if (auto const comparison = left.groupOrderKey.compare(right.groupOrderKey); comparison != 0)
      {
        return comparison < 0;
      }

      if (auto const comparison = left.groupIdentityKey.compare(right.groupIdentityKey); comparison != 0)
      {
        return comparison < 0;
      }

      if (auto const comparison = left.titleKey.compare(right.titleKey); comparison != 0)
      {
        return comparison < 0;
      }

      return left.sourceIndex < right.sourceIndex;
    }

    CachedDictionaryText dictionaryTextCached(boost::unordered_flat_map<std::size_t, CachedDictionaryText>& cache,
                                              rt::detail::StringArena& arena,
                                              std::string& scratch,
                                              Dataset const& dataset,
                                              std::size_t const dictionaryIndex,
                                              TextOrderingPolicy const* const optPolicy,
                                              std::size_t& generatedKeyBytes)
    {
      if (auto const found = cache.find(dictionaryIndex); found != cache.end())
      {
        return found->second;
      }

      auto const& raw = dataset.dictionaryValues.at(dictionaryIndex);
      auto const identityKey = deriveIdentityKey(arena, scratch, raw, optPolicy, generatedKeyBytes);
      auto sortKey = identityKey;

      if (optPolicy != nullptr || stripLeadingArticle(raw).size() != raw.size())
      {
        deriveSortKeyInto(scratch, raw, optPolicy);
        sortKey = internCounted(arena, scratch, generatedKeyBytes);
      }

      auto const value = CachedDictionaryText{.identityKey = identityKey, .sortKey = sortKey};
      cache.emplace(dictionaryIndex, value);
      return value;
    }

    PreparedOrder prepareOrder(Dataset const& dataset, TextOrderingPolicy const* const optPolicy)
    {
      auto prepared = PreparedOrder{};
      prepared.entries.reserve(dataset.titles.size());
      auto dictionaryCache = boost::unordered_flat_map<std::size_t, CachedDictionaryText>{};
      dictionaryCache.reserve(dataset.dictionaryValues.size());
      auto scratch = std::string{};
      scratch.reserve(128);

      for (std::size_t index = 0; index < dataset.titles.size(); ++index)
      {
        auto const dictionaryText = dictionaryTextCached(dictionaryCache,
                                                         *prepared.arenaPtr,
                                                         scratch,
                                                         dataset,
                                                         dataset.dictionaryIndices[index],
                                                         optPolicy,
                                                         prepared.generatedKeyBytes);
        deriveSortKeyInto(scratch, dataset.titles[index], optPolicy);
        prepared.entries.push_back(OrderEntry{
          .groupOrderKey = dictionaryText.sortKey,
          .groupIdentityKey = dictionaryText.identityKey,
          .titleKey = internCounted(*prepared.arenaPtr, scratch, prepared.generatedKeyBytes),
          .sourceIndex = index,
        });
      }

      std::ranges::sort(prepared.entries, lessEntry);
      return prepared;
    }

    OperationResult fullRebuild(Dataset const& dataset, TextOrderingPolicy const* const optPolicy)
    {
      auto prepared = prepareOrder(dataset, optPolicy);
      auto result = OperationResult{.generatedKeyBytes = prepared.generatedKeyBytes};

      for (auto const& entry : prepared.entries)
      {
        result.checksum +=
          entry.sourceIndex + entry.groupOrderKey.size() + entry.groupIdentityKey.size() + entry.titleKey.size();
      }

      return result;
    }

    OperationResult incrementalUpdate(Dataset const& dataset,
                                      PreparedOrder& baseline,
                                      TextOrderingPolicy const* const optPolicy,
                                      std::size_t const sequence)
    {
      auto entries = baseline.entries;
      auto const changedSourceIndex = dataset.titles.size() / 2;
      auto const current = std::ranges::find(entries, changedSourceIndex, &OrderEntry::sourceIndex);

      if (current == entries.end())
      {
        throw std::runtime_error{"incremental performance fixture lost its changed row"};
      }

      auto updated = *current;
      entries.erase(current);
      auto const updatedTitle = std::format("Updated {:04} {}", sequence, dataset.titles[changedSourceIndex]);
      auto scratch = std::string{};
      deriveSortKeyInto(scratch, updatedTitle, optPolicy);
      std::size_t generatedKeyBytes = 0;
      updated.titleKey = internCounted(*baseline.arenaPtr, scratch, generatedKeyBytes);
      auto const insertion = std::ranges::lower_bound(entries, updated, lessEntry);
      entries.insert(insertion, updated);

      return OperationResult{
        .checksum = entries.front().sourceIndex + entries.back().sourceIndex + updated.titleKey.size(),
        .generatedKeyBytes = generatedKeyBytes,
      };
    }

    OperationResult completionVocabulary(Dataset const& dataset, TextOrderingPolicy const* const optPolicy)
    {
      if (optPolicy == nullptr)
      {
        auto values = std::vector<std::string_view>{};
        values.reserve(dataset.dictionaryValues.size());

        for (auto const& value : dataset.dictionaryValues)
        {
          values.push_back(value);
        }

        std::ranges::sort(values);

        auto result = OperationResult{};

        for (auto const value : values)
        {
          result.checksum += value.size();
        }

        return result;
      }

      auto keys = std::vector<std::pair<std::string, std::string_view>>{};
      keys.reserve(dataset.dictionaryValues.size());
      auto result = OperationResult{};

      for (auto const& value : dataset.dictionaryValues)
      {
        keys.emplace_back(std::string{}, value);
        requireSortKeyInto(*optPolicy, keys.back().first, value);
      }

      std::ranges::sort(keys);
      auto previousKey = std::string_view{};
      bool hasPreviousKey = false;

      for (auto const& [key, raw] : keys)
      {
        if (!hasPreviousKey || key != previousKey)
        {
          result.generatedKeyBytes += key.size();
          previousKey = key;
          hasPreviousKey = true;
        }

        result.checksum += key.size() + raw.size();
      }

      return result;
    }

    template<typename Operation>
    Measurement sampleMeasurement(Measurement measurement,
                                  std::size_t const warmups,
                                  std::size_t const samples,
                                  Operation operation)
    {
      for (std::size_t index = 0; index < warmups; ++index)
      {
        std::invoke(operation);
      }

      auto elapsed = std::vector<std::int64_t>{};
      elapsed.reserve(samples);
      std::uint64_t checksum = 0;
      std::size_t generatedKeyBytes = 0;

      for (std::size_t index = 0; index < samples; ++index)
      {
        auto const start = std::chrono::steady_clock::now();
        auto const result = std::invoke(operation);
        auto const finish = std::chrono::steady_clock::now();
        elapsed.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start).count());
        checksum += result.checksum;
        generatedKeyBytes = result.generatedKeyBytes;
      }

      CHECK(checksum != 0);
      setPercentiles(measurement, elapsed);

      if (measurement.optByteMetric)
      {
        measurement.optByteMetric->count = generatedKeyBytes;
      }

      return measurement;
    }

    template<typename Operation>
    Measurement measureOrdering(std::string policy,
                                std::string scenario,
                                std::optional<std::string_view> const optLocale,
                                Dataset const& dataset,
                                std::size_t const warmups,
                                std::size_t const samples,
                                Operation operation,
                                bool const includeByteMetric = true)
    {
      return sampleMeasurement(
        Measurement{
          .capability = "ordering",
          .optPolicy = std::move(policy),
          .scenario = std::move(scenario),
          .optLocale = optLocale.transform([](std::string_view const value) { return std::string{value}; }),
          .dataset = dataset.name,
          .inputCount = dataset.titles.size(),
          .optByteMetric = includeByteMetric ? std::optional{Measurement::ByteMetric{
                                                 .kind = "unique-sort-key-bytes",
                                                 .count = 0,
                                               }}
                                             : std::nullopt,
        },
        warmups,
        samples,
        std::move(operation));
    }

    OperationResult deriveCompletionAliases(CompletionAliasPolicy const& policy, std::string_view const text)
    {
      auto aliases = std::vector<std::string>{};

      if (auto const result = policy.makeAliasesInto(aliases, text); !result)
      {
        throw std::runtime_error{result.error().message};
      }

      auto operation = OperationResult{.checksum = aliases.size()};

      for (auto const& alias : aliases)
      {
        operation.checksum += alias.size();
        operation.generatedKeyBytes += alias.size();
      }

      return operation;
    }

    void invalidateCompletionSnapshot(CompletionReviewHarness& harness)
    {
      auto executionRes =
        runLoopTask(harness.asyncRuntime,
                    harness.executor,
                    executeInteractiveMutation(
                      harness.writeLane.captureSubmission(),
                      [](library::LibraryWrite&) -> Result<OperationOutcome<bool>>
                      { return Changed<bool>{.value = true, .changeSet = LibraryChangeSet{.libraryReset = true}}; }));

      if (!executionRes)
      {
        throw std::runtime_error{executionRes.error().message};
      }
    }

    OperationResult materializeCompletionAliases(CompletionReviewHarness& harness)
    {
      invalidateCompletionSnapshot(harness);
      auto const entries = harness.service.aggregateValues(TrackValueVocabularySpec{
        .fields = kCompletionFields,
        .includeTags = true,
      });
      auto result = OperationResult{.checksum = entries.size() + 1};

      for (auto const& entry : entries)
      {
        result.checksum += entry.value.size() + entry.frequency + entry.aliases.size();

        for (auto const& alias : entry.aliases)
        {
          result.checksum += alias.size();
          result.generatedKeyBytes += alias.size();
        }
      }

      return result;
    }

    OperationResult lookupCompletion(CompletionReviewHarness& harness, std::string_view const prefix)
    {
      auto const optResult = harness.completer.complete(prefix, prefix.size(), kCompletionLookupLimit);
      auto result = OperationResult{.checksum = 1};

      if (!optResult)
      {
        return result;
      }

      result.checksum += optResult->items.size();

      for (auto const& item : optResult->items)
      {
        result.checksum += item.displayText.size() + item.insertText.size() + item.detail.frequency + item.rank;
      }

      return result;
    }

    void populateCompletionLibrary(MusicLibraryFixture& libraryFixture,
                                   std::string_view const dataset,
                                   std::size_t const trackCount)
    {
      auto transaction = library::test::writeTransaction(libraryFixture.library());
      auto populateRes = transaction.apply(
        [&](library::LibraryWrite& write) -> Result<>
        {
          auto writer = write.tracks();

          for (std::size_t index = 0; index < trackCount; ++index)
          {
            auto spec = library::test::TrackSpec{.uri = std::format("completion-review/{:06}.flac", index)};

            if (dataset == "completion-cjk")
            {
              spec.title = std::format("Track CJK {:05}", index);
              spec.artist = std::format("周杰倫 {:04}", index % kCompletionHanValueCount);
              spec.album = std::format("宇多田ヒカル {:03}", index % kCompletionKanaValueCount);
              spec.albumArtist = "宇多田ヒカル";
              spec.genre = "音乐";
            }
            else
            {
              spec.title = std::format("Track ASCII {:05}", index);
              spec.artist = std::format("Artist {:04}", index % (trackCount / 50));
              spec.album = std::format("Album {:04}", index % (trackCount / 100));
              spec.albumArtist = std::format("Album Artist {:03}", index % (trackCount / 500));
              spec.genre = std::format("Genre {:02}", index % 50);
            }

            auto builder = library::TrackBuilder::makeEmpty();
            library::test::applyTrackSpec(builder, spec);

            if (auto createRes = writer.create(builder, library::FileManifestBuilder::makeEmpty()); !createRes)
            {
              return std::unexpected{createRes.error()};
            }
          }

          return {};
        });

      if (!populateRes)
      {
        throw std::runtime_error{populateRes.error().message};
      }

      if (auto commitRes = transaction.commit(); !commitRes)
      {
        throw std::runtime_error{commitRes.error().message};
      }
    }

    template<typename Operation>
    Measurement measureCompletionAlias(std::string policy,
                                       std::string scenario,
                                       std::string dataset,
                                       std::size_t const inputCount,
                                       std::size_t const warmups,
                                       std::size_t const samples,
                                       Operation operation,
                                       bool const includeByteMetric = true)
    {
      return sampleMeasurement(
        Measurement{
          .capability = "completion-alias",
          .optPolicy = std::move(policy),
          .scenario = std::move(scenario),
          .optLocale = std::nullopt,
          .dataset = std::move(dataset),
          .inputCount = inputCount,
          .medianNs = 0,
          .percentile95Ns = 0,
          .optByteMetric = includeByteMetric ? std::optional{Measurement::ByteMetric{
                                                 .kind = "snapshot-alias-bytes",
                                                 .count = 0,
                                               }}
                                             : std::nullopt,
        },
        warmups,
        samples,
        std::move(operation));
    }

    std::unique_ptr<TextOrderingPolicy> requireIcuPolicy(std::string_view const locale)
    {
      auto policyRes = i18n::createIcuTextOrderingPolicy(locale);

      if (!policyRes)
      {
        throw std::runtime_error{policyRes.error().message};
      }

      return std::move(*policyRes);
    }
  } // namespace

  TEST_CASE("PerformanceReview - ordering and completion-alias workloads", "[perf][unit][review]")
  {
    Log::initialize(LogLevel::Info);
    auto const samples = configuredCount("AOBUS_PERF_SAMPLES", kDefaultSamples, 1);
    auto const warmups = configuredCount("AOBUS_PERF_WARMUPS", kDefaultWarmups, 0);
    auto measurements = std::vector<Measurement>{};
    auto const constructionDataset = Dataset{
      .name = "none",
      .titles = {},
      .dictionaryValues = {},
      .dictionaryIndices = {},
    };

    for (auto const locale : {std::string_view{"en-US"}, std::string_view{"de-DE"}, std::string_view{"ja-JP"}})
    {
      measurements.push_back(measureOrdering(
        "icu-secondary",
        "construction",
        std::optional{locale},
        constructionDataset,
        warmups,
        samples,
        [locale]
        {
          auto policyPtr = requireIcuPolicy(locale);
          return OperationResult{.checksum = policyPtr == nullptr ? 0U : 1U};
        },
        false));
    }

    auto const englishPolicyPtr = requireIcuPolicy("en-US");
    auto const germanPolicyPtr = requireIcuPolicy("de-DE");
    auto const japanesePolicyPtr = requireIcuPolicy("ja-JP");

    measurements.push_back(measureCompletionAlias("icu-transliteration",
                                                  "first-kana-use",
                                                  "kana-single",
                                                  1,
                                                  warmups,
                                                  samples,
                                                  []
                                                  {
                                                    auto policyPtr = i18n::createIcuCompletionAliasPolicy();
                                                    return deriveCompletionAliases(*policyPtr, "宇多田ヒカル");
                                                  }));
    measurements.push_back(measureCompletionAlias("icu-transliteration",
                                                  "first-han-use",
                                                  "han-single",
                                                  1,
                                                  warmups,
                                                  samples,
                                                  []
                                                  {
                                                    auto policyPtr = i18n::createIcuCompletionAliasPolicy();
                                                    return deriveCompletionAliases(*policyPtr, "周杰倫");
                                                  }));

    auto const warmKanaAliasPolicyPtr = i18n::createIcuCompletionAliasPolicy();
    auto const warmHanAliasPolicyPtr = i18n::createIcuCompletionAliasPolicy();
    std::ignore = deriveCompletionAliases(*warmKanaAliasPolicyPtr, "宇多田ヒカル");
    std::ignore = deriveCompletionAliases(*warmHanAliasPolicyPtr, "周杰倫");
    measurements.push_back(
      measureCompletionAlias("icu-transliteration",
                             "warm-kana-use",
                             "kana-single",
                             1,
                             warmups,
                             samples,
                             [&] { return deriveCompletionAliases(*warmKanaAliasPolicyPtr, "宇多田ヒカル"); }));
    measurements.push_back(
      measureCompletionAlias("icu-transliteration",
                             "warm-han-use",
                             "han-single",
                             1,
                             warmups,
                             samples,
                             [&] { return deriveCompletionAliases(*warmHanAliasPolicyPtr, "周杰倫"); }));

    for (auto const trackCount : {std::size_t{10000}, std::size_t{50000}})
    {
      for (auto const datasetName : {std::string_view{"ascii"}, std::string_view{"latin"}, std::string_view{"cjk"}})
      {
        auto const dataset = makeDataset(datasetName, trackCount);
        auto prepared = prepareOrder(dataset, nullptr);
        std::size_t byteUpdateSequence = 0;
        measurements.push_back(measureOrdering("ascii-byte",
                                               "full-rebuild",
                                               std::nullopt,
                                               dataset,
                                               warmups,
                                               samples,
                                               [&] { return fullRebuild(dataset, nullptr); }));
        measurements.push_back(
          measureOrdering("ascii-byte",
                          "incremental-update",
                          std::nullopt,
                          dataset,
                          warmups,
                          samples,
                          [&] { return incrementalUpdate(dataset, prepared, nullptr, byteUpdateSequence++); }));
        measurements.push_back(measureOrdering("ascii-byte",
                                               "completion-vocabulary",
                                               std::nullopt,
                                               dataset,
                                               warmups,
                                               samples,
                                               [&] { return completionVocabulary(dataset, nullptr); }));

        TextOrderingPolicy const* icuPolicy = englishPolicyPtr.get();
        auto icuLocale = std::string_view{"en-US"};

        if (datasetName == "cjk")
        {
          icuPolicy = japanesePolicyPtr.get();
          icuLocale = "ja-JP";
        }
        else if (datasetName == "latin")
        {
          icuPolicy = germanPolicyPtr.get();
          icuLocale = "de-DE";
        }

        auto icuPrepared = prepareOrder(dataset, icuPolicy);
        std::size_t icuUpdateSequence = 0;
        measurements.push_back(measureOrdering("icu-secondary",
                                               "full-rebuild",
                                               icuLocale,
                                               dataset,
                                               warmups,
                                               samples,
                                               [&] { return fullRebuild(dataset, icuPolicy); }));
        measurements.push_back(
          measureOrdering("icu-secondary",
                          "incremental-update",
                          icuLocale,
                          dataset,
                          warmups,
                          samples,
                          [&] { return incrementalUpdate(dataset, icuPrepared, icuPolicy, icuUpdateSequence++); }));
        measurements.push_back(measureOrdering("icu-secondary",
                                               "completion-vocabulary",
                                               icuLocale,
                                               dataset,
                                               warmups,
                                               samples,
                                               [&] { return completionVocabulary(dataset, icuPolicy); }));
      }
    }

    for (auto const& [dataset, trackCount] : std::to_array<std::pair<std::string_view, std::size_t>>({
           {"completion-ascii", kCompletionAsciiTrackCount},
           {"completion-cjk", kCompletionCjkTrackCount},
         }))
    {
      auto libraryFixture = MusicLibraryFixture{};
      populateCompletionLibrary(libraryFixture, dataset, trackCount);

      {
        auto harness = CompletionReviewHarness{libraryFixture, nullptr};
        measurements.push_back(measureCompletionAlias(
          "disabled",
          "snapshot-materialization",
          std::string{dataset},
          trackCount,
          warmups,
          samples,
          [&] { return materializeCompletionAliases(harness); },
          false));
      }

      auto aliasPolicyPtr = i18n::createIcuCompletionAliasPolicy();
      auto harness = CompletionReviewHarness{libraryFixture, aliasPolicyPtr.get()};
      measurements.push_back(measureCompletionAlias("icu-transliteration",
                                                    "snapshot-materialization",
                                                    std::string{dataset},
                                                    trackCount,
                                                    warmups,
                                                    samples,
                                                    [&] { return materializeCompletionAliases(harness); }));

      if (dataset == "completion-cjk")
      {
        auto const artists = harness.service.valuesFor(TrackField::Artist);
        auto const aliasedArtists =
          std::ranges::count_if(artists, [](VocabularyEntry const& entry) { return !entry.aliases.empty(); });
        constexpr auto kDirectPrefix = std::string_view{"Track"};
        constexpr auto kWordPrefix = std::string_view{"CJK"};
        constexpr auto kAliasPrefix = std::string_view{"zhoujielun"};
        constexpr auto kMissPrefix = std::string_view{"zzzzzz"};
        auto const optDirect = harness.completer.complete(kDirectPrefix, kDirectPrefix.size(), kCompletionLookupLimit);
        auto const optWord = harness.completer.complete(kWordPrefix, kWordPrefix.size(), kCompletionLookupLimit);
        auto const optAlias = harness.completer.complete(kAliasPrefix, kAliasPrefix.size(), kCompletionLookupLimit);
        auto const optMiss = harness.completer.complete(kMissPrefix, kMissPrefix.size(), kCompletionLookupLimit);
        INFO("artist vocabulary: " << artists.size() << ", aliased artists: " << aliasedArtists);
        REQUIRE(aliasedArtists != 0);
        REQUIRE(optDirect);
        REQUIRE(optWord);
        REQUIRE(optAlias);
        REQUIRE(optDirect->items.size() == kCompletionLookupLimit);
        REQUIRE(optWord->items.size() == kCompletionLookupLimit);
        REQUIRE(optAlias->items.size() == kCompletionLookupLimit);
        REQUIRE_FALSE(optMiss);

        for (auto const& [scenario, prefix] : std::to_array<std::pair<std::string_view, std::string_view>>({
               {"direct-hit", kDirectPrefix},
               {"word-hit", kWordPrefix},
               {"alias-hit", kAliasPrefix},
               {"complete-miss", kMissPrefix},
             }))
        {
          auto measuredScenario = std::string{"cached-lookup-"};
          measuredScenario += scenario;
          measurements.push_back(measureCompletionAlias(
            "icu-transliteration",
            std::move(measuredScenario),
            std::string{dataset},
            trackCount,
            warmups,
            samples,
            [&harness, prefix] { return lookupCompletion(harness, prefix); },
            false));
        }
      }
    }

    std::size_t expectedMeasurementCount = 51;

    if (auto optDataset = makeRealLibraryDataset(); optDataset)
    {
      auto const locale = environmentText("AOBUS_PERF_LIBRARY_LOCALE", "en-US");
      auto const policyPtr = requireIcuPolicy(locale);
      auto bytePrepared = prepareOrder(*optDataset, nullptr);
      auto icuPrepared = prepareOrder(*optDataset, policyPtr.get());
      std::size_t byteUpdateSequence = 0;
      std::size_t icuUpdateSequence = 0;

      measurements.push_back(measureOrdering("ascii-byte",
                                             "full-rebuild",
                                             std::nullopt,
                                             *optDataset,
                                             warmups,
                                             samples,
                                             [&] { return fullRebuild(*optDataset, nullptr); }));
      measurements.push_back(
        measureOrdering("ascii-byte",
                        "incremental-update",
                        std::nullopt,
                        *optDataset,
                        warmups,
                        samples,
                        [&] { return incrementalUpdate(*optDataset, bytePrepared, nullptr, byteUpdateSequence++); }));
      measurements.push_back(measureOrdering("ascii-byte",
                                             "completion-vocabulary",
                                             std::nullopt,
                                             *optDataset,
                                             warmups,
                                             samples,
                                             [&] { return completionVocabulary(*optDataset, nullptr); }));
      measurements.push_back(measureOrdering("icu-secondary",
                                             "full-rebuild",
                                             locale,
                                             *optDataset,
                                             warmups,
                                             samples,
                                             [&] { return fullRebuild(*optDataset, policyPtr.get()); }));
      measurements.push_back(measureOrdering(
        "icu-secondary",
        "incremental-update",
        locale,
        *optDataset,
        warmups,
        samples,
        [&] { return incrementalUpdate(*optDataset, icuPrepared, policyPtr.get(), icuUpdateSequence++); }));
      measurements.push_back(measureOrdering("icu-secondary",
                                             "completion-vocabulary",
                                             locale,
                                             *optDataset,
                                             warmups,
                                             samples,
                                             [&] { return completionVocabulary(*optDataset, policyPtr.get()); }));
      expectedMeasurementCount += 6;
    }

    writeReport(measurements, warmups, samples);
    REQUIRE(measurements.size() == expectedMeasurementCount);
  }

  TEST_CASE("PerformanceReview - query expression admission and recursive phases", "[perf][unit][audit-query]")
  {
    auto const count = configuredCount("AOBUS_AUDIT_QUERY_ATOMS", 128, 1);
    auto const samples = configuredCount("AOBUS_PERF_SAMPLES", kDefaultSamples, 1);
    auto const warmups = configuredCount("AOBUS_PERF_WARMUPS", kDefaultWarmups, 0);
    auto measurements = std::vector<Measurement>{};

    for (auto const* const shape : {"adjacent", "binary", "nested", "quoted", "list"})
    {
      auto input = std::string{};

      if (std::string_view{shape} == "nested")
      {
        input = std::string(count, '(') + "true" + std::string(count, ')');
      }
      else if (std::string_view{shape} == "quoted")
      {
        input = "\"" + std::string(count, 'x') + "\"";
      }
      else
      {
        if (std::string_view{shape} == "list")
        {
          input += '[';
        }

        for (std::size_t index = 0; index < count; ++index)
        {
          if (index != 0)
          {
            if (std::string_view{shape} == "binary")
            {
              input += " and ";
            }
            else if (std::string_view{shape} == "list")
            {
              input += ", ";
            }
            else
            {
              input += ' ';
            }
          }

          input += "true";
        }

        if (std::string_view{shape} == "list")
        {
          input += ']';
        }
      }

      constexpr auto kPhases = std::to_array<std::string_view>({
        "parse-and-normalize",
        "renormalize",
        "compile-query-and-retire-plan",
        "compile-format-and-retire-plan",
        "serialize",
        "ast-teardown",
        "total",
      });
      auto elapsed = std::array<std::vector<std::int64_t>, kPhases.size()>{};

      for (auto& phaseSamples : elapsed)
      {
        phaseSamples.reserve(samples);
      }

      bool accepted = false;
      bool queryAccepted = false;
      bool formatAccepted = false;
      std::size_t outputBytes = 0;

      bool structureAdmitted = true;

      if (std::string_view{shape} == "adjacent")
      {
        structureAdmitted = count <= 512;
      }
      else if (std::string_view{shape} == "binary")
      {
        structureAdmitted = count <= 256;
      }
      else if (std::string_view{shape} == "nested")
      {
        structureAdmitted = count <= 64;
      }

      for (std::size_t run = 0; run < warmups + samples; ++run)
      {
        using Clock = std::chrono::steady_clock;
        auto const start = Clock::now();
        auto previousTime = start;
        auto record = [&](std::size_t const phase)
        {
          if (auto const now = Clock::now(); run >= warmups)
          {
            elapsed[phase].push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(now - previousTime).count());
          }

          previousTime = Clock::now();
        };
        auto expressionRes = query::parse(input);
        record(0);
        accepted = expressionRes.has_value();

        if (accepted)
        {
          query::normalize(*expressionRes);
          record(1);
          queryAccepted = query::compileQuery(*expressionRes).has_value();
          record(2);
          formatAccepted = query::compileFormat(*expressionRes).has_value();
          record(3);
          outputBytes = query::serialize(*expressionRes).size();
          record(4);
          expressionRes = query::Expression{};
          record(5);
        }

        if (run >= warmups)
        {
          elapsed.back().push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
        }

        REQUIRE(accepted == (structureAdmitted && input.size() <= 65536));
      }

      for (std::size_t phase = 0; phase < kPhases.size(); ++phase)
      {
        if (elapsed[phase].empty())
        {
          continue;
        }

        auto measurement = Measurement{
          .capability = "query-admission",
          .scenario = std::format("{}/{}/{}", shape, accepted ? "admitted" : "rejected", kPhases[phase]),
          .dataset = std::format("{} units; {} input bytes; query {}; format {}",
                                 count,
                                 input.size(),
                                 queryAccepted ? "accepted" : "rejected",
                                 formatAccepted ? "accepted" : "rejected"),
          .inputCount = count,
          .optByteMetric = Measurement::ByteMetric{.kind = "serialized-bytes", .count = outputBytes},
        };
        setPercentiles(measurement, elapsed[phase]);
        measurements.push_back(std::move(measurement));
      }
    }

    writeReport(measurements, warmups, samples);
  }

  TEST_CASE("PerformanceReview - graph bursts retain one latest payload until owner delivery",
            "[perf][unit][audit-observation]")
  {
    auto const count = configuredCount("AOBUS_AUDIT_OBSERVATIONS", 1000, 1);
    auto const samples = configuredCount("AOBUS_PERF_SAMPLES", kDefaultSamples, 1);
    auto const warmups = configuredCount("AOBUS_PERF_WARMUPS", kDefaultWarmups, 0);
    auto executor = QueuedExecutor{};
    auto player = audio::Player{executor};
    auto providerPtr = std::make_unique<ObservationProvider>();
    auto* source = providerPtr.get();
    player.addProvider(std::move(providerPtr));
    executor.drain();
    REQUIRE(player.setOutputDevice(audio::kBackendNone, ObservationProvider::device().id, audio::kProfileShared));
    player.handleRouteChanged(
      audio::Engine::RouteStatus{.optAnchor = audio::RouteAnchor{.backend = audio::kBackendNone, .id = "audit-route"}},
      player.playbackGeneration());
    executor.drain();
    std::size_t delivered = 0;
    player.setOnQualityChanged([&delivered](auto const&, bool) { ++delivered; });
    auto graph = audio::flow::Graph{};

    for (std::size_t index = 0; index < 32; ++index)
    {
      graph.nodes.push_back(audio::flow::Node{.id = std::format("node-{}", index), .name = std::string(128, 'x')});
    }

    auto enqueueSamples = std::vector<std::int64_t>{};
    auto drainSamples = std::vector<std::int64_t>{};

    for (std::size_t run = 0; run < warmups + samples; ++run)
    {
      delivered = 0;
      std::int64_t enqueueNs = 0;
      auto producer = std::jthread{
        [&]
        {
          auto const start = std::chrono::steady_clock::now();

          for (std::size_t index = 0; index < count; ++index)
          {
            graph.nodes.back().name = std::format("last-{}", index);
            source->emitGraph(graph);
          }

          enqueueNs =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
        }};
      producer.join();
      CHECK(executor.queuedCount() == 1);
      CHECK(delivered == 0);
      auto const startTime = std::chrono::steady_clock::now();
      REQUIRE(executor.drainUntil([&] { return delivered == 1; }, std::chrono::seconds{30}));
      auto const elapsed =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - startTime).count();
      auto const final = player.status();
      REQUIRE(!final.flow.nodes.empty());
      CHECK(final.flow.nodes.back().name == std::format("last-{}", count - 1));

      if (run >= warmups)
      {
        enqueueSamples.push_back(enqueueNs);
        drainSamples.push_back(elapsed);
      }
    }

    auto const payloadBytes = (32U * sizeof(audio::flow::Node)) + (std::size_t{31} * 129U);
    auto measurements = std::vector<Measurement>{};

    for (auto const* const phase : {"producer-enqueue", "graph-to-quality-drain"})
    {
      auto measurement = Measurement{
        .capability = "audio-observation-retention",
        .scenario = std::format("withheld-owner/{}", phase),
        .dataset =
          std::format("{} emitted graphs; 1 pending delivery; {} delivered callbacks; 32 nodes", count, delivered),
        .inputCount = count,
        .optByteMetric = Measurement::ByteMetric{.kind = "queued-payload-bytes-lower-bound", .count = payloadBytes},
      };
      setPercentiles(measurement, std::string_view{phase} == "producer-enqueue" ? enqueueSamples : drainSamples);
      measurements.push_back(std::move(measurement));
    }

    writeReport(measurements, warmups, samples);
  }
} // namespace ao::rt::test
