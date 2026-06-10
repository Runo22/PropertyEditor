#pragma once

#include <QStyledItemDelegate>

namespace rpe {

class PropertyModel;

// ─────────────────────────────────────────────────────────────────────────────
//  PropertyDelegate — provides type-appropriate inline editors for column 1.
//
//  Editor selection is driven by the value's rttr::type and optional RTTR
//  metadata hints (see rpe/core/EditorHints.h): numbers → spin boxes with
//  min/max/step/decimals, bools → check box, strings → line edit / file picker /
//  multi-line, enums → combo, QColor → color picker.
//
//  Opening an editor implicitly pins the row (override) so live updates don't
//  fight the user mid-edit; committing routes through the model's edit policy.
// ─────────────────────────────────────────────────────────────────────────────
class PropertyDelegate : public QStyledItemDelegate
{
    Q_OBJECT
public:
    explicit PropertyDelegate(PropertyModel* model, QObject* parent = nullptr);

    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem& option,
                          const QModelIndex& index) const override;
    void setEditorData(QWidget* editor, const QModelIndex& index) const override;
    void setModelData(QWidget* editor, QAbstractItemModel* model,
                      const QModelIndex& index) const override;
    void updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& option,
                              const QModelIndex& index) const override;
    void destroyEditor(QWidget* editor, const QModelIndex& index) const override;

private:
    PropertyModel* _model;
    // Track the in-progress edit so a cancelled edit (Esc / focus loss without
    // commit) un-pins a row we pinned in createEditor.
    mutable QString _editPath;
    mutable bool    _editWasOverridden = false;
    mutable bool    _editCommitted     = false;
};

} // namespace rpe
