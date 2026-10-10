// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/fatal/LibraryProbeScenario.h"

#include "lib/library/PhysicalStoreAccess.h"
#include "lib/lmdb/detail/DatabaseOpenAdmissionProbe.h"
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
#include <future>
#include <limits>
#include <memory>
#include <optional>
#include <semaphore>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

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

    std::int32_t writeObservation(std::string_view const observation)
    {
      if (std::fwrite(observation.data(), 1, observation.size(), stdout) != observation.size() ||
          std::fflush(stdout) != 0)
      {
        return 3;
      }

      return 0;
    }

    bool trySeedInvalidIntegerKeyDatabase(std::filesystem::path const& path)
    {
      auto* rawEnvironment = static_cast<MDB_env*>(nullptr);

      if (::mdb_env_create(&rawEnvironment) != MDB_SUCCESS)
      {
        return false;
      }

      auto environmentPtr = std::unique_ptr<MDB_env, decltype(&::mdb_env_close)>{rawEnvironment, &::mdb_env_close};

      if (::mdb_env_set_maxdbs(environmentPtr.get(), 8) != MDB_SUCCESS ||
          ::mdb_env_open(environmentPtr.get(), path.string().c_str(), MDB_NOTLS, 0644) != MDB_SUCCESS)
      {
        return false;
      }

      auto* rawTransaction = static_cast<MDB_txn*>(nullptr);

      if (::mdb_txn_begin(environmentPtr.get(), nullptr, 0, &rawTransaction) != MDB_SUCCESS)
      {
        return false;
      }

      auto transactionPtr = std::unique_ptr<MDB_txn, decltype(&::mdb_txn_abort)>{rawTransaction, &::mdb_txn_abort};
      MDB_dbi database = 0;

      if (::mdb_dbi_open(transactionPtr.get(), "probe", MDB_CREATE | MDB_INTEGERKEY, &database) != MDB_SUCCESS)
      {
        return false;
      }

      auto key = std::string{"xy"};
      auto payload = std::string{"probe"};
      auto nativeKey = MDB_val{.mv_size = key.size(), .mv_data = key.data()};
      auto nativePayload = MDB_val{.mv_size = payload.size(), .mv_data = payload.data()};

      if (::mdb_put(transactionPtr.get(), database, &nativeKey, &nativePayload, MDB_NOOVERWRITE) != MDB_SUCCESS)
      {
        return false;
      }

      return ::mdb_txn_commit(transactionPtr.release()) == MDB_SUCCESS;
    }

    std::int32_t runWriterConflict(std::string_view const scratchName)
    {
      if (scratchName.empty())
      {
        return 3;
      }

      auto const scratchPath = std::filesystem::temp_directory_path() / std::string{scratchName};
      auto libraryRes = MusicLibrary::open(
        scratchPath, scratchPath / "db", MusicLibrary::Options{.pinnedMapBytes = std::size_t{64} * 1024U * 1024U});

      if (!libraryRes)
      {
        std::fputs("Library probe could not open the writer-conflict library\n", stderr);
        return 3;
      }

      auto library = std::move(*libraryRes);

      if (auto writableRes = WritableMusicLibrary::acquire(library);
          writableRes || writableRes.error().code != Error::Code::Conflict)
      {
        std::fputs("Library probe did not observe writer-session conflict\n", stderr);
        return 3;
      }

      return writeObservation("writer-conflict");
    }

    std::int32_t runCommitRevision(std::string_view const scratchName, bool const withDictionary = false)
    {
      if (scratchName.empty())
      {
        return 3;
      }

      auto const scratchPath = std::filesystem::temp_directory_path() / std::string{scratchName};
      auto libraryRes = MusicLibrary::open(
        scratchPath, scratchPath / "db", MusicLibrary::Options{.pinnedMapBytes = std::size_t{64} * 1024U * 1024U});

      if (!libraryRes)
      {
        std::fputs("Library probe could not open the revision library\n", stderr);
        return 3;
      }

      auto library = std::move(*libraryRes);
      auto writableRes = WritableMusicLibrary::acquire(library);

      if (!writableRes)
      {
        std::fputs("Library probe could not acquire the revision writer\n", stderr);
        return 3;
      }

      auto transaction = writableRes->writeTransaction();

      if (withDictionary)
      {
        auto track = TrackBuilder::makeEmpty();
        track.property().uri("child.flac");
        track.metadata().title("Child title").artist("Child artist");
        auto const createdRes = transaction.apply(
          [&track](LibraryWrite& write) { return write.tracks().create(track, FileManifestBuilder::makeEmpty()); });

        if (!createdRes)
        {
          return 3;
        }
      }

      if ((!withDictionary && library.libraryRevision(transaction) != 1) || !transaction.commit())
      {
        std::fputs("Library probe could not commit revision one\n", stderr);
        return 3;
      }

      return writeObservation(withDictionary ? "committed-dictionary" : "committed-revision=1");
    }

    std::int32_t runDefaultReaderCapacity(std::string_view const scratchName)
    {
      if (scratchName.empty())
      {
        return 3;
      }

      auto const scratchPath = std::filesystem::temp_directory_path() / std::string{scratchName};
      auto libraryRes = MusicLibrary::open(
        scratchPath, scratchPath / "db", MusicLibrary::Options{.pinnedMapBytes = std::size_t{16} * 1024U * 1024U});

      if (!libraryRes)
      {
        return 3;
      }

      auto library = std::move(*libraryRes);
      auto transactions = std::vector<ReadTransaction>{};
      transactions.reserve(160);

      for (std::size_t index = 0; index < 160; ++index)
      {
        transactions.push_back(library.readTransaction());
      }

      return writeObservation("readers=160");
    }

    std::int32_t runFreshReaderCapacityExhaustion(std::string_view const scratchName)
    {
      if (scratchName.empty())
      {
        return 3;
      }

      auto const scratchPath = std::filesystem::temp_directory_path() / std::string{scratchName};
      auto libraryRes =
        MusicLibrary::open(scratchPath,
                           scratchPath / "db",
                           MusicLibrary::Options{.maxReaders = 64, .pinnedMapBytes = std::size_t{16} * 1024U * 1024U});

      if (!libraryRes)
      {
        return 3;
      }

      auto library = std::move(*libraryRes);
      auto transactions = std::vector<ReadTransaction>{};
      transactions.reserve(65);

      for (std::size_t index = 0; index < 65; ++index)
      {
        transactions.push_back(library.readTransaction());
      }

      return 3;
    }

    std::int32_t runCrossLibraryFact(std::string_view const scratchName, std::string_view const scenario)
    {
      if (scratchName.empty())
      {
        return 3;
      }

      auto const scratchPath = std::filesystem::temp_directory_path() / std::string{scratchName};
      auto firstRes = MusicLibrary::open(scratchPath,
                                         scratchPath / "first-db",
                                         MusicLibrary::Options{.pinnedMapBytes = std::size_t{16} * 1024U * 1024U});
      auto secondRes = MusicLibrary::open(scratchPath,
                                          scratchPath / "second-db",
                                          MusicLibrary::Options{.pinnedMapBytes = std::size_t{16} * 1024U * 1024U});

      if (!firstRes || !secondRes)
      {
        return 3;
      }

      auto first = std::move(*firstRes);
      auto second = std::move(*secondRes);
      auto writableRes = WritableMusicLibrary::acquire(second);

      if (!writableRes)
      {
        return 3;
      }

      auto transaction = writableRes->writeTransaction();

      if (scenario == "cross-library-write-revision")
      {
        std::ignore = first.libraryRevision(transaction);
        return 3;
      }

      if (scenario == "cross-library-operation-metadata")
      {
        std::ignore = transaction.apply(
          [&first](LibraryWrite& write) -> Result<>
          {
            std::ignore = first.metadataHeader(write);
            return {};
          });
        return 3;
      }

      return 2;
    }

    std::int32_t runZeroListUpdate(std::string_view const scratchName)
    {
      if (scratchName.empty())
      {
        return 3;
      }

      auto const scratchPath = std::filesystem::temp_directory_path() / std::string{scratchName};
      auto libraryRes = MusicLibrary::open(
        scratchPath, scratchPath / "db", MusicLibrary::Options{.pinnedMapBytes = std::size_t{16} * 1024U * 1024U});

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
        scratchPath, scratchPath / "db", MusicLibrary::Options{.pinnedMapBytes = std::size_t{16} * 1024U * 1024U});

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
        scratchPath, scratchPath / "db", MusicLibrary::Options{.pinnedMapBytes = std::size_t{16} * 1024U * 1024U});

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
        scratchPath, scratchPath / "db", MusicLibrary::Options{.pinnedMapBytes = std::size_t{16} * 1024U * 1024U});

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
      auto const removed = exerciseWriter ? detail::PhysicalStoreAccess::tryRemoveHotTrackRecordForTest(
                                              library.tracks(), corruptTransaction, *optTrackId)
                                          : detail::PhysicalStoreAccess::tryRemoveColdTrackRecordForTest(
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
        scratchPath, scratchPath / "db", MusicLibrary::Options{.pinnedMapBytes = std::size_t{16} * 1024U * 1024U});

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
        scratchPath, scratchPath / "db", MusicLibrary::Options{.pinnedMapBytes = std::size_t{16} * 1024U * 1024U});

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
        scratchPath, scratchPath / "db", MusicLibrary::Options{.pinnedMapBytes = std::size_t{16} * 1024U * 1024U});

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
        scratchPath, scratchPath / "db", MusicLibrary::Options{.pinnedMapBytes = std::size_t{16} * 1024U * 1024U});

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
        detail::PhysicalStoreAccess::writer(library.manifest(), corruptTransaction).tryRemove("probe.flac");

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
          scratchPath, databasePath, MusicLibrary::Options{.pinnedMapBytes = std::size_t{16} * 1024U * 1024U});

        if (!libraryRes)
        {
          return 3;
        }
      }

      {
        auto environmentRes = lmdb::Environment::open(
          databasePath,
          lmdb::Environment::Options{
            .flags = lmdb::kEnvNoTls, .maxDatabases = 8, .pinnedMapBytes = std::size_t{16} * 1024U * 1024U});

        if (!environmentRes)
        {
          return 3;
        }

        auto transactionRes = lmdb::WriteTransaction::begin(*environmentRes);

        if (!transactionRes)
        {
          return 3;
        }

        auto metadataRes = lmdb::IntegerKeyDatabase::open(*transactionRes, "meta");
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
        scratchPath, databasePath, MusicLibrary::Options{.pinnedMapBytes = std::size_t{16} * 1024U * 1024U});

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
      auto const invalidIntegerKey = scenario == "lmdb-invalid-integer-key";
      auto const emptyLowerBoundKey = scenario == "lmdb-empty-lower-bound-key";

      if (invalidIntegerKey && !trySeedInvalidIntegerKeyDatabase(scratchPath))
      {
        return 3;
      }

      auto environmentRes =
        lmdb::Environment::open(scratchPath, lmdb::Environment::Options{.flags = lmdb::kEnvNoTls, .maxDatabases = 8});

      if (!environmentRes)
      {
        return 3;
      }

      auto setupRes = lmdb::WriteTransaction::begin(*environmentRes);

      if (!setupRes)
      {
        return 3;
      }

      if (emptyLowerBoundKey)
      {
        auto databaseRes = lmdb::ByteKeyDatabase::open(*setupRes, "probe");

        if (!databaseRes || !setupRes->commit())
        {
          return 3;
        }

        auto readRes = lmdb::ReadTransaction::begin(*environmentRes);

        if (!readRes)
        {
          return 3;
        }

        std::ignore = databaseRes->reader(*readRes).lowerBound(std::span<std::byte const>{});
        return 3;
      }

      auto databaseRes = invalidIntegerKey ? lmdb::IntegerKeyDatabase::openExisting(*setupRes, "probe")
                                           : lmdb::IntegerKeyDatabase::open(*setupRes, "probe");

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
          auto postCommitWriter = databaseRes->writer(*setupRes);
          std::ignore = postCommitWriter.get(1);
          return 3;
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

      if (invalidIntegerKey)
      {
        if (!setupRes->commit())
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

    std::int32_t runDatabaseOpenAdmissionRelease(std::string_view const scratchName, std::string_view const scenario)
    {
      if (scratchName.empty())
      {
        return 3;
      }

      auto const scratchPath = std::filesystem::temp_directory_path() / std::string{scratchName};
      auto const firstPath = scratchPath / "first-environment";
      auto const secondPath = scratchPath / "second-environment";
      auto error = std::error_code{};
      std::filesystem::create_directories(firstPath, error);

      if (error)
      {
        return 3;
      }

      std::filesystem::create_directories(secondPath, error);

      if (error)
      {
        return 3;
      }

      auto firstEnvironmentRes =
        lmdb::Environment::open(firstPath, lmdb::Environment::Options{.flags = lmdb::kEnvNoTls, .maxDatabases = 8});
      auto secondEnvironmentRes =
        lmdb::Environment::open(secondPath, lmdb::Environment::Options{.flags = lmdb::kEnvNoTls, .maxDatabases = 8});

      if (!firstEnvironmentRes || !secondEnvironmentRes)
      {
        return 3;
      }

      auto firstTransactionRes = lmdb::WriteTransaction::begin(*firstEnvironmentRes);

      if (!firstTransactionRes)
      {
        return 3;
      }

      auto firstTransactionPtr = std::make_unique<lmdb::WriteTransaction>(std::move(*firstTransactionRes));

      auto firstDatabaseRes = lmdb::IntegerKeyDatabase::open(*firstTransactionPtr, "first");

      if (!firstDatabaseRes ||
          !firstDatabaseRes->writer(*firstTransactionPtr).create(1, utility::bytes::view(std::string_view{"staged"})))
      {
        return 3;
      }

      auto contentionSignal = std::binary_semaphore{0};
      auto second = std::async(std::launch::async,
                               [&]
                               {
                                 auto transactionRes = lmdb::WriteTransaction::begin(*secondEnvironmentRes);

                                 if (!transactionRes)
                                 {
                                   return false;
                                 }

                                 [[maybe_unused]] auto probe =
                                   lmdb::detail::DatabaseOpenAdmissionProbe{contentionSignal};
                                 auto databaseRes = lmdb::IntegerKeyDatabase::open(*transactionRes, "second");
                                 return databaseRes.has_value() && transactionRes->commit().has_value();
                               });

      contentionSignal.acquire();

      if (scenario == "lmdb-database-open-admission-release-commit")
      {
        if (!firstTransactionPtr->commit())
        {
          return 3;
        }
      }
      else if (scenario == "lmdb-database-open-admission-release-abort")
      {
        firstTransactionPtr->abort();
      }
      else if (scenario == "lmdb-database-open-admission-release-destruction")
      {
        firstTransactionPtr.reset();
      }
      else if (scenario == "lmdb-database-open-admission-release-move-assignment")
      {
        // Replacing by a finished writer must abort before unlocking admission.
        *firstTransactionPtr = std::move(*firstTransactionRes);
      }
      else if (scenario == "lmdb-database-open-admission-release-move-round-trip")
      {
        auto moved = lmdb::WriteTransaction{std::move(*firstTransactionPtr)};
        *firstTransactionPtr = std::move(moved);
        auto* const sameOwner = firstTransactionPtr.get();
        *firstTransactionPtr = std::move(*sameOwner);

        if (!firstTransactionPtr->commit())
        {
          return 3;
        }
      }
      else
      {
        return 2;
      }

      if (!second.get())
      {
        return 3;
      }

      // A fresh writer also proves the first native writer ended. Aborted
      // creation disappears; commit through moved ownership preserves payload.
      // This observes termination after the terminal call returns, not the
      // relative order of native termination and admission unlock inside it.
      auto verificationRes = lmdb::WriteTransaction::begin(*firstEnvironmentRes);

      if (!verificationRes)
      {
        return 3;
      }

      auto databaseRes = lmdb::IntegerKeyDatabase::openExisting(*verificationRes, "first");
      bool const committed = scenario == "lmdb-database-open-admission-release-commit" ||
                             scenario == "lmdb-database-open-admission-release-move-round-trip";

      if (committed)
      {
        if (!databaseRes)
        {
          return 3;
        }

        auto const optValue = databaseRes->reader(*verificationRes).get(1);

        if (!optValue || utility::bytes::stringView(*optValue) != "staged")
        {
          return 3;
        }
      }
      else if (databaseRes || databaseRes.error().code != Error::Code::NotFound)
      {
        return 3;
      }

      return writeObservation(scenario);
    }

    // Creates named integer-key databases with one probe row in the first
    // named database, through the typed wrappers only.
    bool trySeedNamedIntegerKeyDatabases(std::filesystem::path const& path,
                                         std::span<std::string const> names,
                                         lmdb::DbiHandle maxDatabases)
    {
      auto error = std::error_code{};
      std::filesystem::create_directories(path, error);

      if (error)
      {
        return false;
      }

      auto environmentRes = lmdb::Environment::open(
        path, lmdb::Environment::Options{.flags = lmdb::kEnvNoTls, .maxDatabases = maxDatabases});

      if (!environmentRes)
      {
        return false;
      }

      auto transactionRes = lmdb::WriteTransaction::begin(*environmentRes);

      if (!transactionRes)
      {
        return false;
      }

      for (auto const& name : names)
      {
        if (!lmdb::IntegerKeyDatabase::open(*transactionRes, name))
        {
          return false;
        }
      }

      if (auto const firstDatabaseRes = lmdb::IntegerKeyDatabase::openExisting(*transactionRes, names.front());
          !firstDatabaseRes ||
          !firstDatabaseRes->writer(*transactionRes).create(1, utility::bytes::view(std::string_view{"probe"})))
      {
        return false;
      }

      return transactionRes->commit().has_value();
    }

    template<typename Database, typename Key>
    bool hasBindingValue(Database const& database,
                         lmdb::ReadTransaction const& owner,
                         Key const key,
                         std::string_view const expected)
    {
      auto reader = database.reader(owner);
      auto const optValue = reader.get(key);
      auto iterator = reader.begin();
      return optValue && utility::bytes::stringView(*optValue) == expected && reader.entryCount() == 1 &&
             iterator != reader.end() && utility::bytes::stringView(iterator->second) == expected;
    }

    template<typename Database, typename Transaction, typename Key>
    std::int32_t runRetainedBindingLifetime(std::string_view const scratchName,
                                            std::string_view const scenario,
                                            Key const key)
    {
      if (scratchName.empty())
      {
        return 3;
      }

      auto const scratchPath = std::filesystem::temp_directory_path() / std::string{scratchName};
      auto const firstPath = scratchPath / "first";
      auto const secondPath = scratchPath / "second";
      auto error = std::error_code{};
      std::filesystem::create_directories(firstPath, error);

      if (error)
      {
        return 3;
      }

      std::filesystem::create_directories(secondPath, error);

      if (error)
      {
        return 3;
      }

      auto environmentRes = lmdb::Environment::open(firstPath, {.flags = lmdb::kEnvNoTls, .maxDatabases = 2});
      auto replacementEnvironmentRes =
        lmdb::Environment::open(secondPath, {.flags = lmdb::kEnvNoTls, .maxDatabases = 2});

      if (!environmentRes || !replacementEnvironmentRes)
      {
        return 3;
      }

      auto setupRes = lmdb::WriteTransaction::begin(*environmentRes);

      if (!setupRes)
      {
        return 3;
      }

      auto databaseRes = Database::open(*setupRes, "records");

      if (!databaseRes ||
          !databaseRes->writer(*setupRes).create(key, utility::bytes::view(std::string_view{"original"})) ||
          !setupRes->commit())
      {
        return 3;
      }

      auto replacementSetupRes = lmdb::WriteTransaction::begin(*replacementEnvironmentRes);

      if (!replacementSetupRes)
      {
        return 3;
      }

      auto replacementDatabaseRes = Database::open(*replacementSetupRes, "records");

      if (!replacementDatabaseRes ||
          !replacementDatabaseRes->writer(*replacementSetupRes)
             .create(key, utility::bytes::view(std::string_view{"replacement"})) ||
          !replacementSetupRes->commit())
      {
        return 3;
      }

      auto ownerRes = Transaction::begin(*environmentRes);

      if (!ownerRes)
      {
        return 3;
      }

      auto& owner = *ownerRes;
      auto expected = std::string_view{"original"};

      if constexpr (std::is_same_v<Transaction, lmdb::WriteTransaction>)
      {
        if (!databaseRes->writer(owner).update(key, utility::bytes::view(std::string_view{"staged"})))
        {
          return 3;
        }

        expected = "staged";
      }

      // Borrow the read base, but transfer only the complete transaction below.
      auto reader = databaseRes->reader(static_cast<lmdb::ReadTransaction const&>(owner));
      auto iterator = reader.begin();

      if constexpr (std::is_same_v<Database, lmdb::ByteKeyDatabase>)
      {
        if (scenario.ends_with("seek-dereference") || scenario.ends_with("seek-advance"))
        {
          iterator = reader.lowerBound(key);
        }
      }

      // Preserve captured identity through both iterator move operations too.
      auto movedIterator = typename Database::Reader::Iterator{std::move(iterator)};
      iterator = std::move(movedIterator);
      auto optMoved = std::optional<Transaction>{};

      if (scenario.contains("-construction-") || scenario.contains("-round-trip-") ||
          scenario.contains("-move-out-rebind-"))
      {
        optMoved.emplace(std::move(owner));

        if (!hasBindingValue(*databaseRes, *optMoved, key, expected))
        {
          return 3;
        }
      }

      if (scenario.contains("-round-trip-"))
      {
        owner = std::move(*optMoved);

        if (!hasBindingValue(*databaseRes, owner, key, expected))
        {
          return 3;
        }
      }
      else if (scenario.contains("-replacement-") || scenario.contains("-move-out-rebind-"))
      {
        // No native writer acquisition or DBI open while holding admission:
        // both DBIs were committed before either retained-token writer began.
        auto replacementRes = Transaction::begin(*replacementEnvironmentRes);

        if (!replacementRes)
        {
          return 3;
        }

        owner = std::move(*replacementRes);

        if (!hasBindingValue(*replacementDatabaseRes, owner, key, "replacement"))
        {
          return 3;
        }
      }
      else if (!scenario.contains("-construction-"))
      {
        return 2;
      }

      if (scenario.ends_with("-get"))
      {
        std::ignore = reader.get(key);
      }
      else if (scenario.ends_with("-count"))
      {
        std::ignore = reader.entryCount();
      }
      else if (scenario.ends_with("-begin"))
      {
        std::ignore = reader.begin();
      }
      else if (scenario.ends_with("-advance"))
      {
        ++iterator;
      }
      else if (scenario.ends_with("-seek-dereference"))
      {
        std::ignore = *iterator;
      }
      else if (scenario.ends_with("-dereference"))
      {
        std::ignore = iterator->first;
      }
      else if constexpr (std::is_same_v<Database, lmdb::ByteKeyDatabase>)
      {
        if (scenario.ends_with("-lower-bound"))
        {
          std::ignore = reader.lowerBound(key);
        }
      }

      return 3;
    }

    template<typename Writer, typename Key>
    void exerciseStaleWriter(Writer& writer, Key const key, std::string_view const scenario)
    {
      if (scenario.ends_with("-clear"))
      {
        std::ignore = writer.clear();
      }
      else if (scenario.ends_with("-get"))
      {
        std::ignore = writer.get(key);
      }
      else if (scenario.ends_with("-create"))
      {
        std::ignore = writer.create(key, utility::bytes::view(std::string_view{"invalid"}));
      }
      else if (scenario.ends_with("-update"))
      {
        std::ignore = writer.update(key, utility::bytes::view(std::string_view{"invalid"}));
      }
      else if (scenario.ends_with("-delete"))
      {
        std::ignore = writer.tryDelete(key);
      }
      else if constexpr (std::is_same_v<Writer, lmdb::IntegerKeyDatabase::Writer>)
      {
        if (scenario.ends_with("-append"))
        {
          std::ignore = writer.append(utility::bytes::view(std::string_view{"invalid"}));
        }
        else if (scenario.ends_with("-max"))
        {
          std::ignore = writer.maxKey();
        }
      }
    }

    template<typename Database, typename Key>
    std::int32_t runWriterBindingLifetime(std::string_view const scratchName,
                                          std::string_view const scenario,
                                          Key const key)
    {
      if (scratchName.empty())
      {
        return 3;
      }

      auto const scratchPath = std::filesystem::temp_directory_path() / std::string{scratchName};
      auto const firstPath = scratchPath / "first";
      auto const secondPath = scratchPath / "second";
      auto error = std::error_code{};
      std::filesystem::create_directories(firstPath, error);

      if (error)
      {
        return 3;
      }

      std::filesystem::create_directories(secondPath, error);

      if (error)
      {
        return 3;
      }

      auto environmentRes = lmdb::Environment::open(firstPath, {.flags = lmdb::kEnvNoTls, .maxDatabases = 2});
      auto replacementEnvironmentRes =
        lmdb::Environment::open(secondPath, {.flags = lmdb::kEnvNoTls, .maxDatabases = 2});

      if (!environmentRes || !replacementEnvironmentRes)
      {
        return 3;
      }

      auto setupRes = lmdb::WriteTransaction::begin(*environmentRes);

      if (!setupRes)
      {
        return 3;
      }

      auto databaseRes = Database::open(*setupRes, "records");

      if (!databaseRes ||
          !databaseRes->writer(*setupRes).create(key, utility::bytes::view(std::string_view{"original"})) ||
          !setupRes->commit())
      {
        return 3;
      }

      auto replacementSetupRes = lmdb::WriteTransaction::begin(*replacementEnvironmentRes);

      if (!replacementSetupRes)
      {
        return 3;
      }

      auto replacementDatabaseRes = Database::open(*replacementSetupRes, "records");

      if (!replacementDatabaseRes ||
          !replacementDatabaseRes->writer(*replacementSetupRes)
             .create(key, utility::bytes::view(std::string_view{"replacement"})) ||
          !replacementSetupRes->commit())
      {
        return 3;
      }

      // Retained DBIs avoid acquiring another native writer while admission is held.
      auto ownerRes = lmdb::WriteTransaction::begin(*environmentRes);

      if (!ownerRes)
      {
        return 3;
      }

      auto& owner = *ownerRes;
      auto writer = databaseRes->writer(owner);

      if (!writer.update(key, utility::bytes::view(std::string_view{"staged"})))
      {
        return 3;
      }

      if (scenario.contains("-wrapper-construction-"))
      {
        [[maybe_unused]] auto moved = typename Database::Writer{std::move(writer)};
        // NOLINTNEXTLINE(bugprone-use-after-move): deliberately exercise the moved-from wrapper contract.
        exerciseStaleWriter(writer, key, scenario);
        return 3;
      }

      if (scenario.contains("-wrapper-assignment-"))
      {
        auto destination = databaseRes->writer(owner);
        destination = std::move(writer);
        // NOLINTNEXTLINE(bugprone-use-after-move): deliberately exercise the moved-from wrapper contract.
        exerciseStaleWriter(writer, key, scenario);
        return 3;
      }

      // Both wrapper moves must retain the old captured identity, not recapture it.
      auto movedWriter = typename Database::Writer{std::move(writer)};
      writer = std::move(movedWriter);
      auto optMovedOwner = std::optional<lmdb::WriteTransaction>{};
      auto const* currentDatabase = &*databaseRes;
      auto* currentOwner = &owner;
      auto expected = std::string_view{"staged"};

      if (scenario.contains("-committed-rebind-"))
      {
        if (!owner.commit())
        {
          return 3;
        }

        auto replacementRes = lmdb::WriteTransaction::begin(*environmentRes);

        if (!replacementRes)
        {
          return 3;
        }

        owner = std::move(*replacementRes);
      }
      else if (scenario.contains("-round-trip-") || scenario.contains("-construction-") ||
               scenario.contains("-move-out-rebind-"))
      {
        optMovedOwner.emplace(std::move(owner));
        currentOwner = nullptr;

        if (!hasBindingValue(*databaseRes, *optMovedOwner, key, "staged"))
        {
          return 3;
        }

        if (scenario.contains("-round-trip-"))
        {
          // The same native handle returns; only the owner-local generation changes.
          owner = std::move(*optMovedOwner);
          currentOwner = &owner;
        }
      }
      else if (!scenario.contains("-replacement-"))
      {
        return 2;
      }

      if (scenario.contains("-replacement-") || scenario.contains("-move-out-rebind-"))
      {
        auto replacementRes = lmdb::WriteTransaction::begin(*replacementEnvironmentRes);

        if (!replacementRes)
        {
          return 3;
        }

        owner = std::move(*replacementRes);
        currentOwner = &owner;
        currentDatabase = &*replacementDatabaseRes;
        expected = "replacement";
      }

      if (currentOwner != nullptr)
      {
        if (!currentOwner->isActive())
        {
          return 3;
        }

        auto fresh = currentDatabase->writer(*currentOwner);

        if (auto const optValue = fresh.get(key);
            !optValue || utility::bytes::stringView(*optValue) != expected ||
            !fresh.update(key, utility::bytes::view(std::string_view{"fresh binding"})))
        {
          return 3;
        }
      }

      // Moving a stale wrapper must transfer its captured identity unchanged,
      // rather than silently rebinding it to the owner's replacement lifetime.
      auto retainedWriter = typename Database::Writer{std::move(writer)};
      writer = std::move(retainedWriter);
      auto* const sameWriter = &writer;
      writer = std::move(*sameWriter);

      // In particular clear() uses no stale cursor: committed-rebind must fail
      // even without allocator reuse of the native transaction or cursor address.
      exerciseStaleWriter(writer, key, scenario);
      return 3;
    }

    std::int32_t runReadonlyReaderLifetime(std::string_view const scratchName, std::string_view const scenario)
    {
      if (scratchName.empty())
      {
        return 3;
      }

      auto const scratchPath = std::filesystem::temp_directory_path() / std::string{scratchName};
      auto const environmentPath = scratchPath / "environment";

      if (auto const probeNames = std::array<std::string, 1>{"probe"};
          !trySeedNamedIntegerKeyDatabases(environmentPath, probeNames, 8))
      {
        return 3;
      }

      auto environmentRes = lmdb::Environment::open(
        environmentPath, lmdb::Environment::Options{.flags = lmdb::kEnvNoTls, .maxDatabases = 8});

      if (!environmentRes)
      {
        return 3;
      }

      auto setupRes = lmdb::WriteTransaction::begin(*environmentRes);

      if (!setupRes)
      {
        return 3;
      }

      auto databaseRes = lmdb::IntegerKeyDatabase::openExisting(*setupRes, "probe");

      if (!databaseRes || !setupRes->commit())
      {
        return 3;
      }

      auto ownerRes = lmdb::ReadTransaction::begin(*environmentRes);

      if (!ownerRes)
      {
        return 3;
      }

      auto owner = std::move(*ownerRes);
      auto reader = databaseRes->reader(owner);
      auto iterator = reader.begin();

      if (scenario == "lmdb-reader-after-transaction-replacement" ||
          scenario == "lmdb-iterator-after-transaction-replacement")
      {
        // Replace the owning transaction: the owner object stays active under a
        // new native handle, so the readers' captured handles no longer match.
        auto replacementRes = lmdb::ReadTransaction::begin(*environmentRes);

        if (!replacementRes)
        {
          return 3;
        }

        owner = std::move(*replacementRes);
      }
      else if (scenario == "lmdb-reader-after-transaction-move-round-trip" ||
               scenario == "lmdb-iterator-after-transaction-move-round-trip")
      {
        // Move the owner out and back: the exact native handle returns to the
        // owner object, so a handle-only guard would still match. Each move
        // invalidates the binding generation the readers captured, so the
        // stale access below is rejected even though the owner is live again.
        auto moved = lmdb::ReadTransaction{std::move(owner)};
        owner = std::move(moved);

        // The owner is genuinely usable again: a fresh wrapper read succeeds
        // before any stale access is attempted.
        if (!databaseRes->reader(owner).get(1))
        {
          return 3;
        }
      }
      else
      {
        return 2;
      }

      if (scenario == "lmdb-reader-after-transaction-replacement" ||
          scenario == "lmdb-reader-after-transaction-move-round-trip")
      {
        std::ignore = reader.get(1);
      }
      else
      {
        std::ignore = iterator->first;
      }

      return 3;
    }

    std::int32_t runWriterBaseTransfer(std::string_view const scratchName, std::string_view const scenario)
    {
      auto const path = std::filesystem::temp_directory_path() / std::string{scratchName};
      auto environmentRes = lmdb::Environment::open(path, {.flags = lmdb::kEnvNoTls, .maxDatabases = 2});

      if (!environmentRes)
      {
        return 3;
      }

      auto writerRes = lmdb::WriteTransaction::begin(*environmentRes);

      if (!writerRes || !lmdb::IntegerKeyDatabase::open(*writerRes, "probe"))
      {
        return 3;
      }

      auto optMovedWriter = std::optional<lmdb::WriteTransaction>{};

      if (scenario.contains("finished"))
      {
        writerRes->abort();
      }
      else if (scenario.contains("committed"))
      {
        if (!writerRes->commit())
        {
          return 3;
        }
      }
      else if (scenario.contains("moved-from"))
      {
        optMovedWriter.emplace(std::move(*writerRes));
      }

      auto& base = static_cast<lmdb::ReadTransaction&>(*writerRes);

      if (scenario.contains("construct"))
      {
        [[maybe_unused]] auto sliced = lmdb::ReadTransaction{std::move(base)};
      }
      else if (scenario.contains("self"))
      {
        auto* const sameBase = &base;
        base = std::move(*sameBase);
      }
      else
      {
        auto readerRes = lmdb::ReadTransaction::begin(*environmentRes);

        if (!readerRes)
        {
          return 3;
        }

        if (scenario.contains("destination"))
        {
          base = std::move(*readerRes);
        }
        else
        {
          *readerRes = std::move(base);
        }
      }

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
        scratchPath, scratchPath / "db", MusicLibrary::Options{.pinnedMapBytes = std::size_t{16} * 1024U * 1024U});

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

  std::int32_t runLibraryProbeScenario(std::string_view const scenario)
  {
    auto const [name, scratchName] = splitScenario(scenario);

    if (name == "normal-observation")
    {
      if (std::fputs("Library probe diagnostic-only marker\n", stderr) < 0 || std::fflush(stderr) != 0)
      {
        return 3;
      }

      return writeObservation("observation=probe-ready");
    }

    if (name == "oversized-standard-output")
    {
      auto const output = std::string(std::size_t{128} * 1024, 'x');

      if (std::fwrite(output.data(), 1, output.size(), stdout) != output.size() || std::fflush(stdout) != 0)
      {
        return 3;
      }

      return 0;
    }

    if (name == "writer-conflict")
    {
      return runWriterConflict(scratchName);
    }

    if (name == "commit-dictionary")
    {
      return runCommitRevision(scratchName, true);
    }

    if (name == "commit-revision")
    {
      return runCommitRevision(scratchName);
    }

    if (name == "default-reader-capacity")
    {
      return runDefaultReaderCapacity(scratchName);
    }

    if (name == "fresh-reader-capacity-exhaustion")
    {
      return runFreshReaderCapacityExhaustion(scratchName);
    }

    if (name == "cross-library-write-revision" || name == "cross-library-operation-metadata")
    {
      return runCrossLibraryFact(scratchName, name);
    }

    if (name == "list-builder-invalid-view")
    {
      std::ignore = ListBuilder::fromView(ListView{std::span<std::byte const>{}});
    }

    if (name == "manifest-builder-invalid-view")
    {
      std::ignore = FileManifestBuilder::fromView(FileManifestView{std::span<std::byte const>{}});
    }

    if (name == "manifest-bind-zero-track")
    {
      if (auto unboundRes = FileManifestBuilder::makeEmpty().validate("probe.flac"); unboundRes)
      {
        std::ignore = std::move(*unboundRes).bind(kInvalidTrackId);
      }
    }

    if (name == "manifest-bind-consumed")
    {
      if (auto unboundRes = FileManifestBuilder::makeEmpty().validate("probe.flac"); unboundRes)
      {
        std::ignore = std::move(*unboundRes).bind(TrackId{1});
        // NOLINTNEXTLINE(bugprone-use-after-move): the probe deliberately rebinds a consumed manifest.
        std::ignore = std::move(*unboundRes).bind(TrackId{2});
      }
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
        name == "lmdb-invalid-integer-key" || name == "lmdb-empty-lower-bound-key")
    {
      return runLmdbContract(scratchName, name);
    }

    if (name == "lmdb-database-open-admission-release-commit" || name == "lmdb-database-open-admission-release-abort" ||
        name == "lmdb-database-open-admission-release-destruction" ||
        name == "lmdb-database-open-admission-release-move-assignment" ||
        name == "lmdb-database-open-admission-release-move-round-trip")
    {
      return runDatabaseOpenAdmissionRelease(scratchName, name);
    }

    if (name == "lmdb-reader-after-transaction-replacement" || name == "lmdb-iterator-after-transaction-replacement" ||
        name == "lmdb-reader-after-transaction-move-round-trip" ||
        name == "lmdb-iterator-after-transaction-move-round-trip")
    {
      return runReadonlyReaderLifetime(scratchName, name);
    }

    if (name.starts_with("lmdb-binding-integer-read-"))
    {
      return runRetainedBindingLifetime<lmdb::IntegerKeyDatabase, lmdb::ReadTransaction>(
        scratchName, name, std::uint32_t{1});
    }

    if (name.starts_with("lmdb-binding-integer-write-"))
    {
      return runRetainedBindingLifetime<lmdb::IntegerKeyDatabase, lmdb::WriteTransaction>(
        scratchName, name, std::uint32_t{1});
    }

    if (name.starts_with("lmdb-binding-byte-read-"))
    {
      return runRetainedBindingLifetime<lmdb::ByteKeyDatabase, lmdb::ReadTransaction>(
        scratchName, name, utility::bytes::view(std::string_view{"key"}));
    }

    if (name.starts_with("lmdb-binding-byte-write-"))
    {
      return runRetainedBindingLifetime<lmdb::ByteKeyDatabase, lmdb::WriteTransaction>(
        scratchName, name, utility::bytes::view(std::string_view{"key"}));
    }

    if (name.starts_with("lmdb-writer-binding-integer-"))
    {
      return runWriterBindingLifetime<lmdb::IntegerKeyDatabase>(scratchName, name, std::uint32_t{1});
    }

    if (name.starts_with("lmdb-writer-binding-byte-"))
    {
      return runWriterBindingLifetime<lmdb::ByteKeyDatabase>(
        scratchName, name, utility::bytes::view(std::string_view{"key"}));
    }

    if (name.starts_with("lmdb-writer-base-transfer-"))
    {
      return runWriterBaseTransfer(scratchName, name);
    }

    if (name == "nested-apply" || name == "commit-during-apply" || name == "terminated-during-apply" ||
        name == "terminated-during-failed-apply" || name == "terminated-during-throw")
    {
      return runTransactionOperationContract(scratchName, name);
    }

    return 2;
  }
} // namespace ao::library::test
