#pragma once

#include "rpe/core/rttr_prelude.h"

#include "rpe/gui/PropertyModel.h"

#include <QWidget>

#include <functional>


class QTreeView;
class QLineEdit;
class QToolButton;
class QSortFilterProxyModel;

namespace rpe {

class PropertyDelegate;

// ─────────────────────────────────────────────────────────────────────────────
//  PropertyEditor — the reusable, embeddable property grid widget.
//
//  Drop it into a window, a QDockWidget, or a side panel. Bind a type, then feed
//  it live data via refresh()/setPropertyValue(), or make it a data editor by
//  switching to the WriteBack edit policy and giving it an instance provider.
// ─────────────────────────────────────────────────────────────────────────────
class PropertyEditor : public QWidget
{
    Q_OBJECT
public:
    explicit PropertyEditor(QWidget* parent = nullptr);

    // ── Schema / data ────────────────────────────────────────────────────────
    void bindType(rttr::type type);
    void unbind();
    void refresh(const rttr::instance& obj);
    void setPropertyValue(const QString& path, rttr::variant val);

    // ── Behaviour ────────────────────────────────────────────────────────────
    void setReadOnly(bool ro);
    bool isReadOnly() const;

    void setEditPolicy(EditPolicy p);
    EditPolicy editPolicy() const;

    // Object that WriteBack edits target (and that refresh() reads after a write).
    void setInstanceProvider(std::function<rttr::instance()> provider);

    // Convenience for static objects: bind the type and continuously edit `obj`.
    template <class T>
    void editObject(T& obj)
    {
        bindType(rttr::type::get<T>());
        setEditPolicy(EditPolicy::WriteBack);
        setInstanceProvider([ptr = &obj] { return rttr::instance(*ptr); });
        refresh(rttr::instance(obj));
    }

    // ── Chrome ───────────────────────────────────────────────────────────────
    void setToolbarVisible(bool visible);
    void expandAll();

    PropertyModel* model() const { return _model; }
    QTreeView*     view()  const { return _view; }

signals:
    void propertyEdited(const QString& path, const rttr::variant& newValue);

private slots:
    void _onFilterChanged(const QString& text);
    void _onResetAll();
    void _onContextMenu(const QPoint& pos);

private:
    void _setupUi();

    PropertyModel*         _model    = nullptr;
    PropertyDelegate*      _delegate = nullptr;
    QSortFilterProxyModel* _proxy    = nullptr;
    QTreeView*             _view     = nullptr;
    QWidget*               _toolbar  = nullptr;
    QLineEdit*             _filter   = nullptr;
    QToolButton*           _resetBtn = nullptr;
};

} // namespace rpe
