// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <QtWidgets/QStyledItemDelegate>

class TagsDelegate : public QStyledItemDelegate
{
  Q_OBJECT
public:
  using QStyledItemDelegate::QStyledItemDelegate;

  void paint(QPainter* painter, QStyleOptionViewItem const& option, QModelIndex const& index) const override;
  QSize sizeHint(QStyleOptionViewItem const& option, QModelIndex const& index) const override;
  // QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem& option, const QModelIndex& index) const override
  // { return nullptr; } void setEditorData(QWidget* editor, const QModelIndex& index) const override; void
  // setModelData(QWidget* editor, QAbstractItemModel* model, const QModelIndex& index) const override;

private slots:
  // void commitAndCloseEditor();
};
