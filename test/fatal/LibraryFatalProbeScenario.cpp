// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/fatal/LibraryFatalProbeScenario.h"

#include "lib/library/PhysicalStoreAccess.h"
#include "lib/lmdb/detail/ReadFaultInjection.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestView.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListStore.h>
#include <ao/library/ListView.h>
#include <ao/library/ListWriter.h>
#include <ao/library/MetadataLayout.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackWriter.h>
#include <ao/library/WritableMusicLibrary.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>
#include <ao/utility/ByteView.h>

#include <lmdb.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace ao::library::test
{
  namespace
  {
    std::pair<std::string_view, std::string_view> splitScenario(std::string_view const scenario)
    {
      auto const delimiter = scenario.find(':');

      if (delimiter == std::string_view::npos)
      {
        return {scenario, {}};
      }

      return {scenario.substr(0, delimiter), scenario.substr(delimiter + 1U)};
    }

    std::int32_t runZeroListUpdate(std::string_view const scratchName)
    {
      if (scratchName.empty())
      {
        return 3;
      }

      auto const scratchPath = std::filesystem::temp_directory_path() / std::string{scratchName};
      auto libraryRes = MusicLibrary::open(
        scratchPath, scratchPath / "db", MusicLibrary::Options{.mapSize = std::size_t{16} * 1024U * 1024U});

      if (!libraryRes)
      {
        std::fputs("Library fatal probe could not open its scratch library\n", stderr);
        return 3;
      }

      auto library = std::move(*libraryRes);
      auto writableRes = WritableMusicLibrary::acquire(library);

      if (!writableRes)
      {
        std::fputs("Library fatal probe could not acquire its writable library\n", stderr);
        return 3;
      }

      auto transaction = writableRes->writeTransaction();
      auto preparedRes = ListBuilder::makeEmpty().prepare();

      if (!preparedRes)
      {
        std::fputs("Library fatal probe could not prepare its List\n", stderr);
        return 3;
      }

      std::ignore =
        detail::PhysicalStoreAccess::writer(library.lists(), transaction).update(kInvalidListId, *preparedRes);
      return 3;
    }

    std::int32_t runTrackWriterOutsideOperation(std::string_view const scratchName, bool const commitFirst)
    {
      if (scratchName.empty())
      {
        return 3;
      }

      auto const scratchPath = std::filesystem::temp_directory_path() / std::string{scratchName};
      auto libraryRes = MusicLibrary::open(
        scratchPath, scratchPath / "db", MusicLibrary::Options{.mapSize = std::size_t{16} * 1024U * 1024U});

      if (!libraryRes)
      {
        return 3;
      }

      auto library = std::move(*libraryRes);
      auto writableRes = WritableMusicLibrary::acquire(library);

      if (!writableRes)
      {
        return 3;
      }

      auto transaction = writableRes->writeTransaction();
      auto optWriter = std::optional<TrackWriter>{};
      auto operationRes = transaction.apply(
        [&optWriter](LibraryWrite& write) -> Result<>
        {
          optWriter.emplace(write.tracks());
          return {};
        });

      if (!operationRes || !optWriter)
      {
        return 3;
      }

      if (commitFirst && !transaction.commit())
      {
        return 3;
      }

      std::ignore = optWriter->get(TrackId{1});
      return 3;
    }

    std::int32_t runPostOpenNativeReadFailure(std::string_view const scratchName)
    {
      if (scratchName.empty())
      {
        return 3;
      }

      auto const scratchPath = std::filesystem::temp_directory_path() / std::string{scratchName};
      auto libraryRes = MusicLibrary::open(
        scratchPath, scratchPath / "db", MusicLibrary::Options{.mapSize = std::size_t{16} * 1024U * 1024U});

      if (!libraryRes)
      {
        return 3;
      }

      [[maybe_unused]] auto injection = lmdb::detail::ReadFaultInjection{MDB_PANIC};
      std::ignore = libraryRes->readTransaction();
      return 3;
    }

    std::optional<TrackId> createProbeTrack(WritableMusicLibrary& writable)
    {
      auto track = TrackBuilder::makeEmpty();
      track.property().uri("probe.flac");
      auto transaction = writable.writeTransaction();
      auto idRes = transaction.apply([&track](LibraryWrite& write)
                                     { return write.tracks().create(track, FileManifestBuilder::makeEmpty()); });

      if (!idRes || !transaction.commit())
      {
        return std::nullopt;
      }

      return *idRes;
    }

    std::int32_t runPostOpenTrackHalfRow(std::string_view const scratchName, bool const exerciseWriter)
    {
      if (scratchName.empty())
      {
        return 3;
      }

      auto const scratchPath = std::filesystem::temp_directory_path() / std::string{scratchName};
      auto libraryRes = MusicLibrary::open(
        scratchPath, scratchPath / "db", MusicLibrary::Options{.mapSize = std::size_t{16} * 1024U * 1024U});

      if (!libraryRes)
      {
        return 3;
      }

      auto library = std::move(*libraryRes);
      auto writableRes = WritableMusicLibrary::acquire(library);

      if (!writableRes)
      {
        return 3;
      }

      auto const optTrackId = createProbeTrack(*writableRes);

      if (!optTrackId)
      {
        return 3;
      }

      auto corruptTransaction = writableRes->writeTransaction();
      auto const removed = exerciseWriter ? detail::PhysicalStoreAccess::removeHotTrackRecordForTest(
                                              library.tracks(), corruptTransaction, *optTrackId)
                                          : detail::PhysicalStoreAccess::removeColdTrackRecordForTest(
                                              library.tracks(), corruptTransaction, *optTrackId);

      if (!removed || !corruptTransaction.commit())
      {
        return 3;
      }

      if (exerciseWriter)
      {
        auto triggerTransaction = writableRes->writeTransaction();
        std::ignore = detail::PhysicalStoreAccess::writer(library.tracks(), triggerTransaction)
                        .get(*optTrackId, TrackStore::Reader::LoadMode::Both);
        return 3;
      }

      auto read = library.readTransaction();
      std::ignore = library.tracks().reader(read).get(*optTrackId, TrackStore::Reader::LoadMode::Both);
      return 3;
    }

    std::int32_t runUnconsumedReadFaultInjection()
    {
      [[maybe_unused]] auto injection = lmdb::detail::ReadFaultInjection{MDB_PANIC};
      return 3;
    }

    std::int32_t runPostOpenListCorruption(std::string_view const scratchName)
    {
      if (scratchName.empty())
      {
        return 3;
      }

      auto const scratchPath = std::filesystem::temp_directory_path() / std::string{scratchName};
      auto libraryRes = MusicLibrary::open(
        scratchPath, scratchPath / "db", MusicLibrary::Options{.mapSize = std::size_t{16} * 1024U * 1024U});

      if (!libraryRes)
      {
        return 3;
      }

      auto library = std::move(*libraryRes);
      auto writableRes = WritableMusicLibrary::acquire(library);

      if (!writableRes)
      {
        return 3;
      }

      auto preparedRes = ListBuilder::makeEmpty().name("Probe").prepare();

      if (!preparedRes)
      {
        return 3;
      }

      auto createTransaction = writableRes->writeTransaction();
      auto idRes = detail::PhysicalStoreAccess::writer(library.lists(), createTransaction).create(*preparedRes);

      if (!idRes || !createTransaction.commit())
      {
        return 3;
      }

      auto corruptTransaction = writableRes->writeTransaction();
      auto const corruptBytes = std::array{std::byte{0x42}};
      auto corruptRes = corruptTransaction.apply(
        [&library, id = *idRes, &corruptBytes](LibraryWrite& write)
        { return detail::PhysicalStoreAccess::overwriteListRecordForTest(library.lists(), write, id, corruptBytes); });

      if (!corruptRes || !corruptTransaction.commit())
      {
        return 3;
      }

      auto read = library.readTransaction();
      std::ignore = library.lists().reader(read).get(*idRes);
      return 3;
    }

    std::int32_t runPostOpenListParentBreach(std::string_view const scratchName)
    {
      if (scratchName.empty())
      {
        return 3;
      }

      auto const scratchPath = std::filesystem::temp_directory_path() / std::string{scratchName};
      auto libraryRes = MusicLibrary::open(
        scratchPath, scratchPath / "db", MusicLibrary::Options{.mapSize = std::size_t{16} * 1024U * 1024U});

      if (!libraryRes)
      {
        return 3;
      }

      auto library = std::move(*libraryRes);
      auto writableRes = WritableMusicLibrary::acquire(library);

      if (!writableRes)
      {
        return 3;
      }

      auto invalidParentRes = ListBuilder::makeEmpty().name("Invalid parent").parentId(ListId{999}).prepare();

      if (!invalidParentRes)
      {
        return 3;
      }

      auto corruptTransaction = writableRes->writeTransaction();
      auto parentIdRes =
        detail::PhysicalStoreAccess::writer(library.lists(), corruptTransaction).create(*invalidParentRes);

      if (!parentIdRes || !corruptTransaction.commit())
      {
        return 3;
      }

      auto candidate = ListBuilder::makeEmpty().name("Candidate").parentId(*parentIdRes);
      auto triggerTransaction = writableRes->writeTransaction();
      std::ignore =
        triggerTransaction.apply([&candidate](LibraryWrite& write) { return write.lists().create(candidate); });
      return 3;
    }

    std::int32_t runPostOpenListParentCycle(std::string_view const scratchName)
    {
      if (scratchName.empty())
      {
        return 3;
      }

      auto const scratchPath = std::filesystem::temp_directory_path() / std::string{scratchName};
      auto libraryRes = MusicLibrary::open(
        scratchPath, scratchPath / "db", MusicLibrary::Options{.mapSize = std::size_t{16} * 1024U * 1024U});

      if (!libraryRes)
      {
        return 3;
      }

      auto library = std::move(*libraryRes);
      auto writableRes = WritableMusicLibrary::acquire(library);

      if (!writableRes)
      {
        return 3;
      }

      auto first = ListBuilder::makeEmpty().name("First");
      auto createTransaction = writableRes->writeTransaction();
      auto firstIdRes = createTransaction.apply([&first](LibraryWrite& write) { return write.lists().create(first); });

      if (!firstIdRes)
      {
        return 3;
      }

      auto second = ListBuilder::makeEmpty().name("Second").parentId(*firstIdRes);
      auto secondIdRes =
        createTransaction.apply([&second](LibraryWrite& write) { return write.lists().create(second); });

      if (!secondIdRes || !createTransaction.commit())
      {
        return 3;
      }

      auto firstInCycleRes = ListBuilder::makeEmpty().name("First").parentId(*secondIdRes).prepare();

      if (!firstInCycleRes)
      {
        return 3;
      }

      auto corruptTransaction = writableRes->writeTransaction();
      auto corruptRes = corruptTransaction.apply(
        [&library, firstId = *firstIdRes, &firstInCycleRes](LibraryWrite& write)
        {
          return detail::PhysicalStoreAccess::overwriteListRecordForTest(
            library.lists(), write, firstId, firstInCycleRes->bytes());
        });

      if (!corruptRes || !corruptTransaction.commit())
      {
        return 3;
      }

      auto candidate = ListBuilder::makeEmpty().name("Candidate").parentId(*firstIdRes);
      auto triggerTransaction = writableRes->writeTransaction();
      std::ignore =
        triggerTransaction.apply([&candidate](LibraryWrite& write) { return write.lists().create(candidate); });
      return 3;
    }

    std::int32_t runPostOpenManifestBindingBreach(std::string_view const scratchName)
    {
      if (scratchName.empty())
      {
        return 3;
      }

      auto const scratchPath = std::filesystem::temp_directory_path() / std::string{scratchName};
      auto libraryRes = MusicLibrary::open(
        scratchPath, scratchPath / "db", MusicLibrary::Options{.mapSize = std::size_t{16} * 1024U * 1024U});

      if (!libraryRes)
      {
        return 3;
      }

      auto library = std::move(*libraryRes);
      auto writableRes = WritableMusicLibrary::acquire(library);

      if (!writableRes)
      {
        return 3;
      }

      auto track = TrackBuilder::makeEmpty();
      track.property().uri("probe.flac");
      auto createTransaction = writableRes->writeTransaction();
      auto idRes = createTransaction.apply([&track](LibraryWrite& write)
                                           { return write.tracks().create(track, FileManifestBuilder::makeEmpty()); });

      if (!idRes || !createTransaction.commit())
      {
        return 3;
      }

      auto corruptTransaction = writableRes->writeTransaction();
      auto const removed =
        detail::PhysicalStoreAccess::writer(library.manifest(), corruptTransaction).remove("probe.flac");

      if (!removed || !corruptTransaction.commit())
      {
        return 3;
      }

      auto update = TrackBuilder::makeEmpty();
      auto triggerTransaction = writableRes->writeTransaction();
      std::ignore = triggerTransaction.apply([id = *idRes, &update](LibraryWrite& write)
                                             { return write.tracks().updateHot(id, update); });
      return 3;
    }

    std::int32_t runRevisionExhaustion(std::string_view const scratchName)
    {
      if (scratchName.empty())
      {
        return 3;
      }

      auto const scratchPath = std::filesystem::temp_directory_path() / std::string{scratchName};
      auto const databasePath = scratchPath / "db";

      {
        auto libraryRes = MusicLibrary::open(
          scratchPath, databasePath, MusicLibrary::Options{.mapSize = std::size_t{16} * 1024U * 1024U});

        if (!libraryRes)
        {
          return 3;
        }
      }

      {
        auto environmentRes = lmdb::Environment::open(
          databasePath.string(),
          lmdb::Environment::Options{
            .flags = lmdb::kEnvNoTls, .maxDatabases = 8, .mapSize = std::size_t{16} * 1024U * 1024U});

        if (!environmentRes)
        {
          return 3;
        }

        auto transactionRes = lmdb::WriteTransaction::begin(*environmentRes);

        if (!transactionRes)
        {
          return 3;
        }

        auto metadataRes = lmdb::Database::open(*transactionRes, "meta");
        constexpr auto kMaximumValidRevision = std::numeric_limits<std::uint64_t>::max() - 1U;

        if (!metadataRes ||
            !metadataRes->writer(*transactionRes)
               .create(kLibraryRevisionRecordId, utility::bytes::view(kMaximumValidRevision)) ||
            !transactionRes->commit())
        {
          return 3;
        }
      }

      auto libraryRes = MusicLibrary::open(
        scratchPath, databasePath, MusicLibrary::Options{.mapSize = std::size_t{16} * 1024U * 1024U});

      if (!libraryRes)
      {
        return 3;
      }

      auto writableRes = WritableMusicLibrary::acquire(*libraryRes);

      if (!writableRes)
      {
        return 3;
      }

      std::ignore = writableRes->writeTransaction();
      return 3;
    }

    std::int32_t runLmdbContract(std::string_view const scratchName, std::string_view const scenario)
    {
      if (scratchName.empty())
      {
        return 3;
      }

      auto const scratchPath = std::filesystem::temp_directory_path() / std::string{scratchName};
      auto environmentRes = lmdb::Environment::open(
        scratchPath.string(), lmdb::Environment::Options{.flags = lmdb::kEnvNoTls, .maxDatabases = 8});

      if (!environmentRes)
      {
        return 3;
      }

      auto setupRes = lmdb::WriteTransaction::begin(*environmentRes);

      if (!setupRes)
      {
        return 3;
      }

      auto const keyKind =
        scenario == "lmdb-invalid-integer-key" ? lmdb::Database::KeyKind::Blob : lmdb::Database::KeyKind::Integer;
      auto databaseRes = lmdb::Database::open(*setupRes, "probe", keyKind);

      if (!databaseRes)
      {
        return 3;
      }

      if (scenario == "lmdb-writer-after-commit" || scenario == "lmdb-writer-from-finished")
      {
        auto writer = databaseRes->writer(*setupRes);

        if (!writer.create(1, utility::bytes::view(std::string_view{"probe"})) || !setupRes->commit())
        {
          return 3;
        }

        if (scenario == "lmdb-writer-from-finished")
        {
          std::ignore = databaseRes->writer(*setupRes);
        }

        std::ignore = writer.get(1);
        return 3;
      }

      if (scenario == "lmdb-reader-after-write-commit" || scenario == "lmdb-iterator-after-write-commit")
      {
        auto writer = databaseRes->writer(*setupRes);

        if (!writer.create(1, utility::bytes::view(std::string_view{"probe"})))
        {
          return 3;
        }

        auto reader = databaseRes->reader(*setupRes);
        auto iterator = reader.begin();

        if (!setupRes->commit())
        {
          return 3;
        }

        if (scenario == "lmdb-reader-after-write-commit")
        {
          std::ignore = reader.get(1);
        }

        std::ignore = iterator->first;
        return 3;
      }

      if (scenario == "lmdb-invalid-integer-key")
      {
        auto writer = databaseRes->writer(*setupRes);

        if (!writer.create(
              utility::bytes::view(std::string_view{"xy"}), utility::bytes::view(std::string_view{"probe"})) ||
            !setupRes->commit())
        {
          return 3;
        }

        auto readRes = lmdb::ReadTransaction::begin(*environmentRes);

        if (!readRes)
        {
          return 3;
        }

        auto iterator = databaseRes->reader(*readRes).begin();
        std::ignore = static_cast<std::uint32_t>(iterator->first);
        return 3;
      }

      if (!setupRes->commit())
      {
        return 3;
      }

      auto sourceRes = lmdb::ReadTransaction::begin(*environmentRes);

      if (!sourceRes)
      {
        return 3;
      }

      [[maybe_unused]] auto destination = lmdb::ReadTransaction{std::move(*sourceRes)};
      std::ignore = databaseRes->reader(*sourceRes);
      return 3;
    }

    std::int32_t runTransactionOperationContract(std::string_view const scratchName, std::string_view const scenario)
    {
      if (scratchName.empty())
      {
        return 3;
      }

      auto const scratchPath = std::filesystem::temp_directory_path() / std::string{scratchName};
      auto libraryRes = MusicLibrary::open(
        scratchPath, scratchPath / "db", MusicLibrary::Options{.mapSize = std::size_t{16} * 1024U * 1024U});

      if (!libraryRes)
      {
        return 3;
      }

      auto library = std::move(*libraryRes);
      auto writableRes = WritableMusicLibrary::acquire(library);

      if (!writableRes)
      {
        return 3;
      }

      auto transaction = writableRes->writeTransaction();

      if (scenario == "nested-apply")
      {
        std::ignore = transaction.apply(
          [&transaction](LibraryWrite&) -> Result<>
          {
            return transaction.apply([](LibraryWrite&) -> Result<> { return {}; });
          });
        return 3;
      }

      if (scenario == "commit-during-apply")
      {
        std::ignore = transaction.apply([&transaction](LibraryWrite&) -> Result<> { return transaction.commit(); });
        return 3;
      }

      if (scenario == "terminated-during-apply")
      {
        std::ignore = transaction.apply(
          [&transaction](LibraryWrite&) -> Result<>
          {
            transaction.abort();
            return {};
          });
        return 3;
      }

      if (scenario == "terminated-during-failed-apply")
      {
        std::ignore = transaction.apply(
          [&transaction](LibraryWrite&) -> Result<>
          {
            transaction.abort();
            return makeError(Error::Code::InvalidState, "probe failure");
          });
        return 3;
      }

      if (scenario == "terminated-during-throw")
      {
        std::ignore = transaction.apply(
          [&transaction](LibraryWrite&) -> Result<>
          {
            transaction.abort();
            throw std::runtime_error{"probe failure"};
          });
        return 3;
      }

      return 2;
    }
  } // namespace

  std::int32_t runLibraryFatalProbeScenario(std::string_view const scenario)
  {
    auto const [name, scratchName] = splitScenario(scenario);

    if (name == "list-builder-invalid-view")
    {
      std::ignore = ListBuilder::fromView(ListView{std::span<std::byte const>{}});
    }

    if (name == "manifest-builder-invalid-view")
    {
      std::ignore = FileManifestBuilder::fromView(FileManifestView{std::span<std::byte const>{}});
    }

    if (name == "list-prepared-output-size")
    {
      auto preparedRes = ListBuilder::makeEmpty().prepare();

      if (!preparedRes)
      {
        return 3;
      }

      auto output = std::array<std::byte, 1>{};
      preparedRes->writeTo(output);
    }

    if (name == "manifest-prepared-output-size")
    {
      auto preparedRes = FileManifestBuilder::makeEmpty().trackId(TrackId{1}).prepare("probe.flac");

      if (!preparedRes)
      {
        return 3;
      }

      auto output = std::array<std::byte, 1>{};
      preparedRes->writeTo(output);
    }

    if (name == "list-store-zero-update")
    {
      return runZeroListUpdate(scratchName);
    }

    if (name == "track-writer-after-commit")
    {
      return runTrackWriterOutsideOperation(scratchName, true);
    }

    if (name == "track-writer-after-apply")
    {
      return runTrackWriterOutsideOperation(scratchName, false);
    }

    if (name == "post-open-native-read-failure")
    {
      return runPostOpenNativeReadFailure(scratchName);
    }

    if (name == "post-open-track-reader-half-row")
    {
      return runPostOpenTrackHalfRow(scratchName, false);
    }

    if (name == "post-open-track-writer-half-row")
    {
      return runPostOpenTrackHalfRow(scratchName, true);
    }

    if (name == "lmdb-read-fault-injection-unconsumed")
    {
      return runUnconsumedReadFaultInjection();
    }

    if (name == "post-open-list-corruption")
    {
      return runPostOpenListCorruption(scratchName);
    }

    if (name == "post-open-list-parent-breach")
    {
      return runPostOpenListParentBreach(scratchName);
    }

    if (name == "post-open-list-parent-cycle")
    {
      return runPostOpenListParentCycle(scratchName);
    }

    if (name == "post-open-manifest-binding-breach")
    {
      return runPostOpenManifestBindingBreach(scratchName);
    }

    if (name == "revision-exhaustion")
    {
      return runRevisionExhaustion(scratchName);
    }

    if (name == "lmdb-reader-moved-from" || name == "lmdb-writer-after-commit" || name == "lmdb-writer-from-finished" ||
        name == "lmdb-reader-after-write-commit" || name == "lmdb-iterator-after-write-commit" ||
        name == "lmdb-invalid-integer-key")
    {
      return runLmdbContract(scratchName, name);
    }

    if (name == "nested-apply" || name == "commit-during-apply" || name == "terminated-during-apply" ||
        name == "terminated-during-failed-apply" || name == "terminated-during-throw")
    {
      return runTransactionOperationContract(scratchName, name);
    }

    return 2;
  }
} // namespace ao::library::test
