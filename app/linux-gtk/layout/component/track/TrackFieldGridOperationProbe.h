// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

namespace ao::gtk::layout
{
  class LayoutComponent;

  namespace detail
  {
    /** Source-private observation of field-grid submission settlement. */
    class TrackFieldGridOperationProbe final
    {
    public:
      /**
       * Synchronously observes the live field grid on its GTK owner executor.
       * Includes its built-in and custom metadata submission workflows, not the
       * nested credits editor or a separate detail Undo controller.
       *
       * Observing true after admission, then false without owner retirement or
       * cancellation, witnesses task termination after frontend completion.
       * An empty scope after cancellation is not a settlement witness.
       *
       * The component must remain alive throughout each call. This observation
       * retains no owner or callback and never schedules, mutates or cancels work.
       */
      static bool hasPendingSubmissions(LayoutComponent const& component);
    };
  } // namespace detail
} // namespace ao::gtk::layout
