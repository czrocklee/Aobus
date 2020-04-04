/*
 * Copyright (C) <year> <name of author>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of  MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "NewListDialog.h"

NewListDialog::NewListDialog(QWidget *parent, Qt::WindowFlags f)
  : QDialog{parent, f}
{
  setupUi(this);
}

rs::ml::fbs::ListT NewListDialog::list() const
{
  rs::ml::fbs::ListT l;
  l.name = lineEditName->text().toStdString();
  l.desc = lineEditDesc->text().toStdString();
  l.expr = lineEditExpr->text().toStdString();
  return l;
}
