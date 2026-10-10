// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "PerformanceAllocation.h"
#include "PerformanceReport.h"
#include "runtime/RuntimeOperationProbe.h"
#include "runtime/source/SmartListEvaluator.h"
#include "runtime/source/SmartListSource.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include "test/unit/runtime/source/TrackSourceTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/LoopExecutor.h>
#include <ao/library/Credits.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/RecordingDate.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackWriter.h>
#include <ao/rt/PlaybackLaunchSpec.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/TrackRow.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/completion/CompletionService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibrarySnapshot.h>
#include <ao/rt/projection/TrackListProjection.h>
#include <ao/rt/source/TrackSourceLease.h>
#include <ao/utility/ThreadName.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bitset>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <semaphore>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    constexpr std::size_t kTrackCount = 10000;
    constexpr auto kKinds = std::to_array({library::CreditKind::Conductor,
                                           library::CreditKind::Ensemble,
                                           library::CreditKind::Soloist,
                                           library::CreditKind::Performer});

    enum class CreditShape : std::uint8_t
    {
      Empty,
      Single,
      Many,
      AllKinds,
      Duplicates,
      NonfirstRoles,
    };

    constexpr auto kShapes = std::to_array<std::pair<CreditShape, std::string_view>>({
      {CreditShape::Empty, "empty"},
      {CreditShape::Single, "one"},
      {CreditShape::Many, "many-32"},
      {CreditShape::AllKinds, "all-kinds"},
      {CreditShape::Duplicates, "duplicates-cross-kind"},
      {CreditShape::NonfirstRoles, "nonfirst-roles"},
    });

    std::vector<library::Credit> fixtureCredits(CreditShape const shape)
    {
      if (shape == CreditShape::Empty)
      {
        return {};
      }

      auto entries =
        std::vector<library::Credit>{{.name = "Conductor primary credit", .kind = library::CreditKind::Conductor}};

      if (shape == CreditShape::Single)
      {
        return entries;
      }

      if (shape == CreditShape::Duplicates)
      {
        entries.push_back(entries.front());
        entries.push_back({.name = "Shared participant", .kind = library::CreditKind::Ensemble, .role = "Shared role"});
        entries.push_back({.name = "Shared participant", .kind = library::CreditKind::Soloist, .role = "Shared role"});
        entries.push_back({.name = "Shared participant", .role = "Shared role"});
        entries.push_back(entries.back());
        return entries;
      }

      entries.push_back(
        {.name = "Conductor secondary credit", .kind = library::CreditKind::Conductor, .role = "Role only sentinel"});

      if (shape == CreditShape::Many)
      {
        for (std::size_t index = 2; index < 32; ++index)
        {
          entries.push_back({.name = std::format("Conductor member {:02}", index),
                             .kind = library::CreditKind::Conductor,
                             .role = std::format("Instrument role {:02}", index % 8)});
        }
      }
      else if (shape == CreditShape::AllKinds)
      {
        entries.push_back({.name = "Ensemble primary credit", .kind = library::CreditKind::Ensemble});
        entries.push_back({.name = "Soloist primary credit", .kind = library::CreditKind::Soloist, .role = "Piano"});
        entries.push_back({.name = "Shared participant", .role = "Violin"});
      }

      return entries;
    }

    struct CreditsFixture final
    {
      ao::test::TempDir tempDir;
      library::MusicLibrary storage{
        ao::test::requireValue(library::MusicLibrary::open(tempDir.path(),
                                                           tempDir.path(),
                                                           {.pinnedMapBytes = std::uint64_t{2} * 1024 * 1024 * 1024}))};
      std::vector<TrackId> ids;
    };

    void seed(CreditsFixture& fixture, std::span<library::Credit const> const entries)
    {
      auto transaction = library::test::writeTransaction(fixture.storage);
      auto seedRes = transaction.apply(
        [&](library::LibraryWrite& write) -> Result<>
        {
          auto writer = write.tracks();

          for (std::size_t index = 0; index < kTrackCount; ++index)
          {
            auto const spec = library::test::TrackSpec{
              .title = std::format("Title {:06}", index),
              .recordingDate = {.year = 1960, .month = 2},
              .credits = {entries.begin(), entries.end()},
              .uri = std::format("credits-baseline/{:06}.flac", index),
            };
            auto builder = library::TrackBuilder::makeEmpty();
            library::test::applyTrackSpec(builder, spec);
            auto createRes = writer.create(builder, library::FileManifestBuilder::makeEmpty());

            if (!createRes)
            {
              return std::unexpected{createRes.error()};
            }

            fixture.ids.push_back(*createRes);
          }

          return {};
        });
      REQUIRE(seedRes);
      REQUIRE(transaction.commit());
      REQUIRE(fixture.ids.size() == kTrackCount);
    }

    // Timings are sampled with counting disabled. A separate extra operation
    // records allocations, never validation, fixture setup, or retirement.
    // Replacement new still runs in timing samples, so comparisons require the
    // same instrumentation binary/configuration. No speedup is asserted.
    template<typename Prepare, typename Operation, typename Validate>
    void measure(std::vector<Measurement>& measurements,
                 Measurement measurement,
                 std::size_t const warmups,
                 std::size_t const samples,
                 Prepare prepare,
                 Operation operation,
                 Validate validate)
    {
      auto elapsed = std::vector<std::int64_t>{};
      elapsed.reserve(samples);

      for (std::size_t run = 0; run < warmups + samples; ++run)
      {
        prepare();
        auto const start = std::chrono::steady_clock::now();
        operation();
        auto const duration =
          std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
        validate();

        if (run >= warmups)
        {
          elapsed.push_back(duration);
        }
      }

      prepare();
      auto allocationScope = AllocationSampleScope{};
      operation();
      auto const allocation = allocationScope.finish();
      validate();
      measurement.optAllocationMetric = Measurement::AllocationMetric{
        .scope = "one-extra-operation/all-threads-including-harness/replaced-cxx-new-only/no-malloc-native-mmap-rss/"
                 "excludes-post-reply-cleanup",
        .calls = allocation.calls,
        .requestedBytes = allocation.requestedBytes,
      };
      setPercentiles(measurement, elapsed);
      measurements.push_back(std::move(measurement));
    }

    void measurePrimary(CreditsFixture& fixture,
                        Library& library,
                        std::span<library::Credit const> const entries,
                        std::string const& dataset,
                        std::vector<Measurement>& measurements,
                        std::size_t const warmups,
                        std::size_t const samples)
    {
      struct Primary final
      {
        std::array<DictionaryId, 4> names{};
        std::array<std::size_t, 4> counts{};
        bool operator==(Primary const&) const = default;
      };

      auto expected = Primary{};

      for (auto const& entry : entries)
      {
        if (auto const index = static_cast<std::size_t>(entry.kind); expected.counts[index]++ == 0)
        {
          auto const optId = fixture.storage.dictionary().findId(entry.name);
          REQUIRE(optId);
          expected.names[index] = *optId;
        }
      }

      // Preallocated output excludes vector growth from physical span access.
      auto primary = std::vector<Primary>(fixture.ids.size());
      std::size_t visited = 0;
      measure(
        measurements,
        {.capability = "credits-primary",
         .scenario = "physical-first-id-and-count",
         .dataset = dataset,
         .inputCount = fixture.ids.size()},
        warmups,
        samples,
        [&] { visited = 0; },
        [&]
        {
          auto transaction = fixture.storage.readTransaction();
          auto reader = fixture.storage.tracks().reader(transaction);
          reader.visitTracks(fixture.ids,
                             library::TrackStore::Reader::LoadMode::Cold,
                             [&](TrackId, library::TrackView const& view)
                             {
                               auto& value = primary[visited++];
                               auto const performance = view.performance();

                               for (std::size_t index = 0; index < kKinds.size(); ++index)
                               {
                                 auto const segment = performance.credits(kKinds[index]);
                                 value.names[index] = segment.empty() ? kInvalidDictionaryId : segment.front().nameId;
                                 value.counts[index] = segment.size();
                               }
                             });
        },
        [&]
        {
          REQUIRE(visited == fixture.ids.size());

          for (auto const& value : primary)
          {
            CHECK(value == expected);
          }
        });

      auto rows = std::vector<TrackRow>{};
      measure(
        measurements,
        {.capability = "credits-primary",
         .scenario = "compact-row-materialization",
         .dataset = dataset,
         .inputCount = fixture.ids.size()},
        warmups,
        samples,
        [&] { rows = {}; },
        [&]
        {
          auto snapshot = library.snapshot();
          rows.reserve(fixture.ids.size());

          for (auto const id : fixture.ids)
          {
            if (auto optRow = snapshot.trackRow(id); optRow)
            {
              rows.push_back(std::move(*optRow));
            }
          }
        },
        [&]
        {
          REQUIRE(rows.size() == fixture.ids.size());

          for (std::size_t index = 0; index < rows.size(); ++index)
          {
            auto const& row = rows[index];
            CHECK(row.id == fixture.ids[index]);
            CHECK(row.title == std::format("Title {:06}", index));
            CHECK(row.conductor == fixture.storage.dictionary().getOrDefault(expected.names[0]));
            CHECK(row.ensemble == fixture.storage.dictionary().getOrDefault(expected.names[1]));
            CHECK(row.soloist == fixture.storage.dictionary().getOrDefault(expected.names[2]));
            CHECK(row.conductorCount == expected.counts[0]);
            CHECK(row.ensembleCount == expected.counts[1]);
            CHECK(row.soloistCount == expected.counts[2]);
          }

          // Release capacity outside the clock, so the next run is fresh.
          auto retired = std::vector<TrackRow>{};
          rows.swap(retired);
        });
    }

    void measureQueries(CreditsFixture& fixture,
                        CreditShape const shape,
                        std::string const& dataset,
                        std::vector<Measurement>& measurements,
                        std::size_t const warmups,
                        std::size_t const samples)
    {
      struct Query final
      {
        std::string_view scenario;
        std::string_view expression;
        bool matches;
      };

      bool const hasSecondary =
        shape == CreditShape::Many || shape == CreditShape::AllKinds || shape == CreditShape::NonfirstRoles;
      bool const hasOtherKinds = shape == CreditShape::AllKinds || shape == CreditShape::Duplicates;
      auto const queries = std::to_array<Query>({
        {"all-credit-existence", "$credit?", shape != CreditShape::Empty},
        {"conductor-nonfirst-equality", "$conductor = 'Conductor secondary credit'", hasSecondary},
        {"conductor-nonfirst-substring", "$conductor ~ 'SECONDARY'", hasSecondary},
        {"ensemble-existence", "$ensemble?", hasOtherKinds},
        {"soloist-existence", "$soloist?", hasOtherKinds},
        {"performer-membership", "$performer in ['Shared participant', 'Unresolved member']", hasOtherKinds},
        {"credit-negative-membership", "$credit != 'Conductor secondary credit'", !hasSecondary},
        {"credit-role-is-not-name", "$credit = 'Role only sentinel'", false},
      });
      auto const reversed = std::vector<TrackId>{fixture.ids.rbegin(), fixture.ids.rend()};
      auto sourcePtr = makeMutableTrackSource(reversed);

      for (auto const& query : queries)
      {
        auto evaluator = SmartListEvaluator{fixture.storage};
        auto filtered = SmartListSource{TrackSourceLease{sourcePtr}, evaluator};
        auto const expected = query.matches ? reversed : std::vector<TrackId>{};
        measure(
          measurements,
          {.capability = "credits-query",
           .scenario = std::string{query.scenario},
           .dataset = dataset,
           .inputCount = fixture.ids.size()},
          warmups,
          samples,
          [] {},
          [&]
          {
            filtered.setExpression(std::string{query.expression});
            filtered.reload();
          },
          [&]
          {
            REQUIRE_FALSE(filtered.hasError());
            CHECK(sourceTrackIds(filtered) == expected);
          });
      }

      auto projectionPtr = std::unique_ptr<TrackListProjection>{};
      measure(
        measurements,
        {.capability = "credits-primary",
         .scenario = "conductor-title-sort-fresh-cache",
         .dataset = dataset,
         .inputCount = fixture.ids.size()},
        warmups,
        samples,
        [] {},
        [&]
        {
          projectionPtr = std::make_unique<TrackListProjection>(
            kInvalidViewId,
            TrackSourceLease{sourcePtr},
            fixture.storage,
            TrackOrderSpec{.sortBy = {{TrackSortField::Conductor}, {TrackSortField::Title}}});
        },
        [&]
        {
          REQUIRE(projectionPtr->size() == fixture.ids.size());

          // Equal primaries (including missing) tie by Title, independent
          // of reverse source order and nonfirst credits/roles.
          for (std::size_t index = 0; index < fixture.ids.size(); ++index)
          {
            CHECK(projectionPtr->trackIdAt(index) == fixture.ids[index]);
            CHECK(projectionPtr->indexOf(fixture.ids[index]) == index);
          }

          projectionPtr.reset();
        });
    }

    using Vocabulary = std::map<std::string, std::uint32_t>;

    void requireVocabulary(std::span<VocabularyEntry const> const actual, Vocabulary const& expected)
    {
      auto observed = Vocabulary{};

      for (auto const& entry : actual)
      {
        REQUIRE(observed.emplace(entry.value, entry.frequency).second);
      }

      CHECK(observed == expected);
    }

    void measureCompletion(CreditsFixture& fixture,
                           LibraryChanges& changes,
                           std::span<library::Credit const> const entries,
                           std::string const& dataset,
                           std::vector<Measurement>& measurements,
                           std::size_t const warmups,
                           std::size_t const samples)
    {
      auto names = Vocabulary{};
      auto roles = Vocabulary{};
      auto categories = std::array<Vocabulary, 4>{};

      // Homogeneous fixtures give each distinct value exactly one vote per
      // track, regardless of duplicates within or across kinds.
      for (auto const& entry : entries)
      {
        names[entry.name] = kTrackCount;
        categories[static_cast<std::size_t>(entry.kind)][entry.name] = kTrackCount;

        if (!entry.role.empty())
        {
          roles[entry.role] = kTrackCount;
        }
      }

      auto servicePtr = std::unique_ptr<CompletionService>{};
      auto vocabularies = std::array<std::span<VocabularyEntry const>, 9>{};
      auto read = [&]
      {
        vocabularies[0] = servicePtr->creditNames();
        vocabularies[1] = servicePtr->creditRoles();

        for (std::size_t index = 0; index < kKinds.size(); ++index)
        {
          vocabularies[2 + index] = servicePtr->creditNames(kKinds[index]);
        }

        vocabularies[6] = servicePtr->valuesFor(TrackField::Conductor);
        vocabularies[7] = servicePtr->valuesFor(TrackField::Ensemble);
        vocabularies[8] = servicePtr->valuesFor(TrackField::Soloist);
      };
      auto validate = [&]
      {
        requireVocabulary(vocabularies[0], names);
        requireVocabulary(vocabularies[1], roles);

        for (std::size_t index = 0; index < kKinds.size(); ++index)
        {
          requireVocabulary(vocabularies[2 + index], categories[index]);
        }

        for (std::size_t index = 0; index < 3; ++index)
        {
          requireVocabulary(vocabularies[6 + index], categories[index]);
        }
      };
      measure(
        measurements,
        {.capability = "credits-completion",
         .scenario = "fresh-snapshot-all-vocabularies",
         .dataset = dataset,
         .inputCount = fixture.ids.size()},
        warmups,
        samples,
        [&]
        {
          vocabularies = {};
          servicePtr = std::make_unique<CompletionService>(fixture.storage, changes);
        },
        read,
        validate);
      measure(
        measurements,
        {.capability = "credits-completion",
         .scenario = "cached-all-vocabularies",
         .dataset = dataset,
         .inputCount = fixture.ids.size()},
        warmups,
        samples,
        [] {},
        read,
        validate);
    }

    void measureScopedEdits(CreditsFixture& fixture,
                            LibraryCommandsFixture& commands,
                            LibraryChanges& changes,
                            std::span<library::Credit const> const entries,
                            std::string const& dataset,
                            std::vector<Measurement>& measurements,
                            std::size_t const warmups,
                            std::size_t const samples)
    {
      auto const targets = std::span<TrackId const>{fixture.ids}.first(32);
      auto baseline = std::vector<library::Credit>{entries.begin(), entries.end()};
      auto originalConductors = std::vector<library::Credit>{};
      auto retained = std::vector<library::Credit>{};

      for (auto const& entry : entries)
      {
        (entry.kind == library::CreditKind::Conductor ? originalConductors : retained).push_back(entry);
      }

      // Give selected tracks independently retained ensemble roles. A scoped
      // conductor command must not broadcast the first target's other segments.
      auto retainedByTarget = std::vector<std::vector<library::Credit>>(targets.size(), retained);

      for (std::size_t index = 0; index < targets.size(); ++index)
      {
        auto ensemble = std::vector<library::Credit>{};

        for (auto& entry : retainedByTarget[index])
        {
          if (entry.kind == library::CreditKind::Ensemble)
          {
            entry.role = std::format("Target-specific ensemble role {:02}", index);
            ensemble.push_back(entry);
          }
        }

        if (!ensemble.empty())
        {
          auto updateRes = commands.updateMetadata(
            targets.subspan(index, 1),
            MetadataPatch{.optCredits = CreditReplacement{
                            .kinds = std::bitset<library::kCreditKindCount>{0b0010}, .entries = std::move(ensemble)}});
          REQUIRE(updateRes);
        }
      }

      auto const restore =
        MetadataPatch{.optCredits = CreditReplacement{
                        .kinds = std::bitset<library::kCreditKindCount>{0b0001}, .entries = originalConductors}};
      struct Edit final
      {
        std::string_view scenario;
        std::vector<library::Credit> authored;
        std::vector<library::Credit> expected;
      };
      auto const replacement = std::vector<library::Credit>{
        {.name = "Replacement conductor", .kind = library::CreditKind::Conductor, .role = "Replacement role"}};
      auto edits = std::vector<Edit>{
        {"scoped-conductor-replace-binding-through-reply", replacement, replacement},
        {"scoped-conductor-clear-binding-through-reply", {}, {}},
        {"scoped-normalized-noop-binding-through-reply", originalConductors, originalConductors},
      };

      for (auto& entry : edits.back().authored)
      {
        entry.name = " " + entry.name + "\t";
        entry.role = " " + entry.role + "\t";
      }

      if (originalConductors.size() > 1)
      {
        auto roleOnly = originalConductors;
        roleOnly[1].role = "Changed nonfirst role";
        edits.push_back({"scoped-nonfirst-role-binding-through-reply", roleOnly, roleOnly});
      }

      std::size_t publications = 0;
      auto subscription = changes.onChanged([&](LibraryChangeSet const&) { ++publications; });

      for (auto const& edit : edits)
      {
        auto expected = edit.expected;
        expected.append_range(retained);
        bool const isChanged = expected != baseline;
        auto const patch =
          MetadataPatch{.optCredits = CreditReplacement{
                          .kinds = std::bitset<library::kCreditKindCount>{0b0001}, .entries = edit.authored}};
        auto replyRes = Result<UpdateTrackMetadataReply>{};
        std::uint64_t revision = 0;
        std::size_t beforePublications = 0;
        measure(
          measurements,
          {.capability = "credits-edit",
           .scenario = std::string{edit.scenario},
           .dataset = dataset + "/targets=32",
           .inputCount = targets.size()},
          warmups,
          samples,
          [&]
          {
            REQUIRE(commands.updateMetadata(targets, restore));
            replyRes = UpdateTrackMetadataReply{};
            revision = commands.library().snapshot().revision();
            beforePublications = publications;
          },
          [&] { replyRes = commands.updateMetadata(targets, patch); },
          [&]
          {
            REQUIRE(replyRes);
            CHECK(replyRes->changes.size() == (isChanged ? targets.size() : 0));
            CHECK(publications == beforePublications + (isChanged ? 1 : 0));
            auto snapshot = commands.library().snapshot();
            CHECK(snapshot.revision() == revision + (isChanged ? 1 : 0));

            for (std::size_t index = 0; index < targets.size(); ++index)
            {
              auto const optCredits = snapshot.trackCredits(targets[index]);
              REQUIRE(optCredits);
              auto expectedForTarget = edit.expected;
              expectedForTarget.append_range(retainedByTarget[index]);
              CHECK(*optCredits == expectedForTarget);
              auto const optRow = snapshot.trackRow(targets[index]);
              REQUIRE(optRow);
              CHECK(optRow->title == std::format("Title {:06}", index));
              CHECK(optRow->recordingDate == library::RecordingDate{.year = 1960, .month = 2});
            }

            auto const optUntouched = snapshot.trackCredits(fixture.ids.back());
            REQUIRE(optUntouched);
            CHECK(*optUntouched == baseline);
          });
      }
    }
  } // namespace

  TEST_CASE("CreditsBaseline - representative credit reads queries completion and scoped edits",
            "[perf][integration][credits-metadata]")
  {
    auto const samples = configuredCount("AOBUS_PERF_SAMPLES", 20, 1);
    auto const warmups = configuredCount("AOBUS_PERF_WARMUPS", 1, 0);
    auto measurements = std::vector<Measurement>{};
    measurements.push_back(
      {.capability = "credits-size",
       .scenario = "compact-track-row-sizeof",
       .dataset = "abi-structure-only/not-heap-or-rss",
       .inputCount = 1,
       .optByteMetric = Measurement::ByteMetric{.kind = "sizeof-track-row", .count = sizeof(TrackRow)}});

    auto const sizes = rt::detail::RuntimeOperationProbe::projectionStorageSizes();

    for (auto const& [scenario, bytes] : std::to_array<std::pair<std::string_view, std::size_t>>({
           {"sort-keys-sizeof", sizes.sortKeys},
           {"order-entry-sizeof", sizes.orderEntry},
           {"dictionary-cache-entry-sizeof", sizes.dictionaryCacheEntry},
         }))
    {
      REQUIRE(bytes > 0);
      measurements.push_back(
        {.capability = "credits-size",
         .scenario = std::string{scenario},
         .dataset = "abi-structure-only/excludes-arena-payload-map-buckets-heap-rss",
         .inputCount = 1,
         .optByteMetric = Measurement::ByteMetric{.kind = "sizeof-actual-projection-type", .count = bytes}});
    }

    for (auto const& [shape, label] : kShapes)
    {
      CAPTURE(label);
      auto const entries = fixtureCredits(shape);
      auto fixture = CreditsFixture{};
      seed(fixture, entries);
      auto executor = async::LoopExecutor{};
      auto changes = makeLibraryChanges(executor, fixture.storage);
      auto commands = LibraryCommandsFixture{fixture.storage, changes, executor};
      auto const dataset = std::format(
        "credits-v1/library={}/shape={}/entries={}/page-cache=seeded-warm", fixture.ids.size(), label, entries.size());
      measurePrimary(fixture, commands.library(), entries, dataset, measurements, warmups, samples);
      measureQueries(fixture, shape, dataset, measurements, warmups, samples);
      measureCompletion(fixture, changes, entries, dataset, measurements, warmups, samples);
      measureScopedEdits(fixture, commands, changes, entries, dataset, measurements, warmups, samples);
    }

    REQUIRE(measurements.size() == 104);
    writeReport(measurements, warmups, samples);
  }

  TEST_CASE("PerformanceAllocation - ordinary aligned sized and worker allocation paths are counted",
            "[perf][unit][allocation-counter][concurrency]")
  {
    // Direct calls cannot be removed via new-expression allocation elision.
    // Worker creation/teardown and Catch assertions are outside the window.
    auto ready = std::binary_semaphore{0};
    auto start = std::binary_semaphore{0};
    auto done = std::binary_semaphore{0};
    auto worker = std::jthread{[&]
                               {
                                 setCurrentThreadName("perf-alloc");
                                 ready.release();
                                 start.acquire();
                                 auto* memory = ::operator new(37);
                                 ::operator delete(memory);
                                 done.release();
                               }};
    ready.acquire();
    auto scope = AllocationSampleScope{};
    auto* scalar = ::operator new(11);
    auto* array = ::operator new[](13);
    auto* aligned = ::operator new(17, std::align_val_t{64});
    auto* alignedArray = ::operator new[](19, std::align_val_t{64});
    auto* nothrow = ::operator new(23, std::nothrow);
    auto* nothrowArray = ::operator new[](29, std::nothrow);
    auto* alignedNothrow = ::operator new(31, std::align_val_t{64}, std::nothrow);
    auto* alignedNothrowArray = ::operator new[](41, std::align_val_t{64}, std::nothrow);
    ::operator delete(scalar, std::size_t{11});
    ::operator delete[](array, std::size_t{13});
    ::operator delete(aligned, std::size_t{17}, std::align_val_t{64});
    ::operator delete[](alignedArray, std::size_t{19}, std::align_val_t{64});
    ::operator delete(nothrow, std::nothrow);
    ::operator delete[](nothrowArray, std::nothrow);
    ::operator delete(alignedNothrow, std::align_val_t{64}, std::nothrow);
    ::operator delete[](alignedNothrowArray, std::align_val_t{64}, std::nothrow);
    start.release();
    done.acquire();
    auto const sample = scope.finish();
    worker.join();
    CHECK(sample.calls == 9);
    CHECK(sample.requestedBytes == 221);
  }
} // namespace ao::rt::test
