// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "app/macos-appkit/LibraryEditorModel.h"
#include "test/MessageCatalogTestSupport.h"
#include "test/TestFixtureSupport.h"
#include "test/runtime/AppRuntimeTestSupport.h"
#include "test/runtime/ExecutorTestSupport.h"
#include "test/runtime/RuntimeLibraryTestSupport.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibrarySnapshot.h>

#include <catch2/catch_test_macros.hpp>
#include <gsl-lite/gsl-lite.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>
#include <memory>
#include <utility>

namespace ao::appkit::test
{
  TEST_CASE("LibraryEditorModel - shutdown preserves a committed Properties edit with publication pending",
            "[concurrency]")
  {
    auto tempDir = ao::test::TempDir{};
    auto executorPtr = std::make_unique<rt::test::ManualExecutor>();
    auto* const executor = executorPtr.get();
    auto runtimePtr = rt::test::makeRuntime(tempDir, std::move(executorPtr));
    auto& runtime = *runtimePtr;
    auto& library = runtime.library();
    auto const target = rt::test::addRuntimeTrack(runtime, {.title = "Target", .genre = "Before"});
    auto const control = rt::test::addRuntimeTrack(runtime, {.title = "Control", .genre = "Untouched"});
    executor->runUntilIdle();
    auto const revision = library.snapshot().revision();

    std::size_t publications = 0;
    auto const subscription = library.changes().onChanged([&](rt::LibraryChangeSet const&) { ++publications; });
    std::size_t editorChanges = 0;
    auto model = LibraryEditorModel{
      library, runtime.async(), runtime.notifications(), ao::test::englishMessageCatalog(), [&] { ++editorChanges; }};
    REQUIRE(model.beginProperties(std::array{target}));
    auto const field = std::ranges::find(
      model.state().fields, rt::TrackField::Genre, [](LibraryEditorField const& row) { return row.spec.field; });
    REQUIRE(field != model.state().fields.end());
    model.editField(static_cast<std::size_t>(std::distance(model.state().fields.begin(), field)), "Committed");
    std::size_t changesAtSave = 0;

    auto const checkCommittedRows = [&]
    {
      auto const snapshot = library.snapshot();
      auto const optTarget = snapshot.trackRow(target);
      auto const optControl = snapshot.trackRow(control);
      REQUIRE(optTarget);
      REQUIRE(optControl);
      CHECK(optTarget->genre == "Committed");
      CHECK(optControl->genre == "Untouched");
      CHECK(snapshot.revision() == revision + 1);
    };

    {
      auto const closeOnExit = gsl_lite::finally(
        [&]
        {
          model.shutdown();
          runtime.shutdown();
          executor->runUntilIdle();
        });
      model.save();
      changesAtSave = editorChanges;
      REQUIRE(model.state().busy);

      // Run only the hop that admits the write. The next turn is publication,
      // queued by the write lane after the transaction becomes durable.
      REQUIRE(executor->tryWaitUntilQueued());
      REQUIRE(executor->tryRunOne());
      REQUIRE(executor->tryWaitUntilQueued());
      REQUIRE(executor->queuedCount() == 1);
      checkCommittedRows();
      REQUIRE(model.state().busy);
      CHECK_FALSE(model.state().completed);
      CHECK(publications == 0);
      CHECK(editorChanges == changesAtSave);
    }

    CHECK(executor->queuedCount() == 0);
    CHECK(publications == 0);
    CHECK(editorChanges == changesAtSave);
    CHECK_FALSE(model.state().completed);
    checkCommittedRows();
  }
} // namespace ao::appkit::test
