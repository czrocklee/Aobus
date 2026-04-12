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

  ListId getSourceListId() const { return _sourceListId; }
  void setSourceListId(ListId id) { _sourceListId = id; }

  int getDepth() const { return _depth; }
  void setDepth(int depth) { _depth = depth; }

  bool isSmart() const { return _isSmart; }
  void setSmart(bool smart) { _isSmart = smart; }

  Glib::ustring getName() const { return _name; }
  void setName(Glib::ustring const& name) { _name = name; }

  static Glib::RefPtr<ListRow> create(ListId id,
                                      ListId sourceListId,
                                      int depth,
                                      bool smart,
                                      Glib::ustring const& name);

protected:
  explicit ListRow();

private:
  ListId _listId;
  ListId _sourceListId;
  int _depth = 0;
  bool _isSmart = false;
  Glib::ustring _name;
};
