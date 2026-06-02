#pragma once

#include <QStyledItemDelegate>

namespace rpe {

class PropertyModel;

class PropertyDelegate : public QStyledItemDelegate
{
    Q_OBJECT

public:
    explicit PropertyDelegate(PropertyModel* model, QObject* parent = nullptr);

    QWidget* createEditor(QWidget*                    parent,
                          const QStyleOptionViewItem& option,
                          const QModelIndex&          index) const override;

    void setEditorData(QWidget*           editor,
                       const QModelIndex& index) const override;

    void setModelData(QWidget*            editor,
                      QAbstractItemModel* model,
                      const QModelIndex&  index) const override;

    void updateEditorGeometry(QWidget*                    editor,
                              const QStyleOptionViewItem& option,
                              const QModelIndex&          index) const override;

    void paint(QPainter*                   painter,
               const QStyleOptionViewItem& option,
               const QModelIndex&          index) const override;

private:
    PropertyModel* _model;
};

} // namespace rpe
