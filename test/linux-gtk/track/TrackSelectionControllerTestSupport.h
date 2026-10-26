// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>

#include <gdkmm/graphene_point.h>

namespace Gtk
{
  class ColumnView;
} // namespace Gtk

namespace ao::gtk::test
{
  /// Appends a plain fixed-width track column whose cells carry the row's
  /// track id, mirroring the row cells the selection controller activates.
  void appendTestColumn(Gtk::ColumnView& columnView);

  /// Appends a cell column shaped like the tags column: the tags CSS class is
  /// what the selection controller's tags-cell double-click path picks on.
  void appendTagsTestColumn(Gtk::ColumnView& columnView);

  /// Finds the cell label bound to @p trackId whose text is @p cellText and
  /// returns its center in @p columnView coordinates, the point real pointer
  /// input or synthetic gesture emission must target. The label must be
  /// mapped with positive width and height, and the coordinate transform must
  /// produce a valid point; otherwise the call fails the enclosing test case.
  Gdk::Graphene::Point cellCenterInView(Gtk::ColumnView& columnView, char const* cellText, TrackId trackId);
} // namespace ao::gtk::test
