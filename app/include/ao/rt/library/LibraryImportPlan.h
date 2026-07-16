// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/rt/library/LibraryYamlImporter.h>

#include <memory>

namespace ao::rt
{
  class LibraryTaskService;

  /**
   * One-shot import authorization bound to one validated source snapshot and
   * one target-library identity and revision.
   */
  class LibraryImportPlan final
  {
  public:
    ~LibraryImportPlan();

    LibraryImportPlan(LibraryImportPlan const&) = delete;
    LibraryImportPlan& operator=(LibraryImportPlan const&) = delete;
    LibraryImportPlan(LibraryImportPlan&&) noexcept;
    LibraryImportPlan& operator=(LibraryImportPlan&&) noexcept;

    /** Returns the prepared report while this plan has not been moved from. */
    ImportReport const& report() const noexcept;

  private:
    struct Impl;
    explicit LibraryImportPlan(std::unique_ptr<Impl> implPtr);

    std::unique_ptr<Impl> _implPtr;

    friend class LibraryTaskService;
  };
} // namespace ao::rt
