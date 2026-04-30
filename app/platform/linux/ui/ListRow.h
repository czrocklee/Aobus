// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/library/MusicLibrary.h>

#include <cstdint>
#include <gtkmm.h>

#include <string>

namespace app::ui
{

  class ListRow final : public Glib::Object
  {
  public:
    using ListId = rs::ListId;

    ListId getListId() const { return _listId; }
    void setListId(ListId id) { _listId = id; }

    ListId getParentId() const { return _parentId; }
    void setParentId(ListId id) { _parentId = id; }

    std::int32_t getDepth() const { return _depth; }
    void setDepth(std::int32_t depth) { _depth = depth; }

    bool isSmart() const { return _isSmart; }
    void setSmart(bool smart) { _isSmart = smart; }

    Glib::ustring getName() const { return _name; }
    void setName(Glib::ustring const& name) { _name = name; }

    Glib::ustring getFilter() const { return _filter; }

    static Glib::RefPtr<ListRow> create(ListId id,
                                        ListId sourceListId,
                                        std::int32_t depth,
                                        bool smart,
                                        Glib::ustring const& name,
                                        Glib::ustring const& filter = "");

  protected:
    explicit ListRow();

  private:
    ListId _listId;
    ListId _parentId;
    std::int32_t _depth = 0;
    bool _isSmart = false;
    Glib::ustring _name;
    Glib::ustring _filter;
  };

} // namespace app::ui
