#pragma once

#include <rs/core/MusicLibrary.h>

#include <gtkmm.h>

#include <string>

class ListRow : public Glib::Object
{
public:
  using ListId = rs::core::MusicLibrary::ListId;

  ListId getListId() const { return _listId; }
  void setListId(ListId id) { _listId = id; }

  Glib::ustring getName() const { return _name; }
  void setName(const Glib::ustring& name) { _name = name; }

  static Glib::RefPtr<ListRow> create(ListId id, const Glib::ustring& name);

protected:
  explicit ListRow();

public:
  ListId _listId;
  Glib::ustring _name;
};
