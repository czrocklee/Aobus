#include "ListRow.h"

ListRow::ListRow() : _listId{ListId{0}}, _name{} {}

Glib::RefPtr<ListRow> ListRow::create(ListId id, const Glib::ustring& name)
{
  auto obj = Glib::make_refptr_for_instance<ListRow>(new ListRow());
  obj->_listId = id;
  obj->_name = name;
  return obj;
}
