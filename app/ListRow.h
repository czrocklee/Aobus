// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/core/MusicLibrary.h>

#include <gtkmm.h>

#include <string>

class ListRow final : public Glib::Object
{
public:
  using ListId = rs::core::ListId;

  ListId getListId() const { return _listId; }
  void setListId(ListId id) { _listId = id; }

  Glib::ustring getName() const { return _name; }
  void setName(Glib::ustring const& name) { _name = name; }

  static Glib::RefPtr<ListRow> create(ListId id, Glib::ustring const& name);

protected:
  explicit ListRow();

public:
  ListId _listId;
  Glib::ustring _name;
};
