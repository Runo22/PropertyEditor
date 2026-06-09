#include "rpe/gui/PropertyEditor.h"

#include "rpe/gui/PropertyDelegate.h"

#include <QAction>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLineEdit>
#include <QMenu>
#include <QSortFilterProxyModel>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>

namespace rpe {

PropertyEditor::PropertyEditor(QWidget* parent)
    : QWidget(parent)
{
    _model    = new PropertyModel(this);
    _delegate = new PropertyDelegate(_model, this);
    _setupUi();
    connect(_model, &PropertyModel::propertyEdited, this, &PropertyEditor::propertyEdited);
}

void PropertyEditor::_setupUi()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(2);

    // toolbar
    _toolbar = new QWidget(this);
    auto* tb = new QHBoxLayout(_toolbar);
    tb->setContentsMargins(4, 2, 4, 2);
    tb->setSpacing(4);

    _filter = new QLineEdit(_toolbar);
    _filter->setPlaceholderText(tr("Filter properties…"));
    _filter->setClearButtonEnabled(true);
    tb->addWidget(_filter, 1);

    _resetBtn = new QToolButton(_toolbar);
    _resetBtn->setText(tr("Reset"));
    _resetBtn->setToolTip(tr("Release all pinned/overridden values"));
    tb->addWidget(_resetBtn);

    root->addWidget(_toolbar);

    // proxy for filtering
    _proxy = new QSortFilterProxyModel(this);
    _proxy->setSourceModel(_model);
    _proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
    _proxy->setFilterKeyColumn(0);
    _proxy->setRecursiveFilteringEnabled(true);

    _view = new QTreeView(this);
    _view->setModel(_proxy);
    _view->setItemDelegateForColumn(1, _delegate);
    _view->setEditTriggers(QAbstractItemView::DoubleClicked
                         | QAbstractItemView::SelectedClicked
                         | QAbstractItemView::EditKeyPressed);
    _view->setAlternatingRowColors(true);
    _view->setUniformRowHeights(true);
    _view->setAnimated(false);                 // keep redraws cheap under live updates
    _view->setExpandsOnDoubleClick(false);
    _view->header()->setStretchLastSection(true);
    _view->header()->resizeSection(0, 200);
    _view->setContextMenuPolicy(Qt::CustomContextMenu);
    root->addWidget(_view, 1);

    connect(_filter,   &QLineEdit::textChanged,   this, &PropertyEditor::_onFilterChanged);
    connect(_resetBtn, &QToolButton::clicked,     this, &PropertyEditor::_onResetAll);
    connect(_view,     &QTreeView::customContextMenuRequested,
            this,      &PropertyEditor::_onContextMenu);
}

// ── data / schema ────────────────────────────────────────────────────────────

void PropertyEditor::bindType(rttr::type type)
{
    _model->bindType(type);
    _view->expandToDepth(0);
}

void PropertyEditor::unbind()                                   { _model->unbind(); }
void PropertyEditor::refresh(const rttr::instance& obj)         { _model->refresh(obj); }
void PropertyEditor::setPropertyValue(const QString& p, rttr::variant v)
{ _model->setPropertyValue(p, std::move(v)); }

// ── behaviour ─────────────────────────────────────────────────────────────────

void PropertyEditor::setReadOnly(bool ro)
{
    _model->setReadOnly(ro);
    _resetBtn->setEnabled(!ro);
}
bool PropertyEditor::isReadOnly() const { return _model->isReadOnly(); }

void PropertyEditor::setEditPolicy(EditPolicy p) { _model->setEditPolicy(p); }
EditPolicy PropertyEditor::editPolicy() const    { return _model->editPolicy(); }

void PropertyEditor::setInstanceProvider(std::function<rttr::instance()> provider)
{ _model->setInstanceProvider(std::move(provider)); }

// ── chrome ──────────────────────────────────────────────────────────────────

void PropertyEditor::setToolbarVisible(bool visible) { _toolbar->setVisible(visible); }
void PropertyEditor::expandAll()                     { _view->expandAll(); }

// ── slots ─────────────────────────────────────────────────────────────────────

void PropertyEditor::_onFilterChanged(const QString& text)
{
    _proxy->setFilterFixedString(text);
    if (text.isEmpty()) _view->expandToDepth(0);
    else                _view->expandAll();
}

void PropertyEditor::_onResetAll() { _model->resetAll(); }

void PropertyEditor::_onContextMenu(const QPoint& pos)
{
    const QModelIndex proxyIdx = _view->indexAt(pos);
    if (!proxyIdx.isValid()) return;
    const QModelIndex srcIdx = _proxy->mapToSource(proxyIdx);
    if (!srcIdx.isValid()) return;

    const QString path       = srcIdx.data(PropertyPathRole).toString();
    const bool    overridden = srcIdx.data(IsOverriddenRole).toBool();

    QMenu menu(this);
    if (!isReadOnly()) {
        if (!overridden) {
            connect(menu.addAction(tr("Pin / Override value")), &QAction::triggered,
                    this, [this, path] { _model->overrideNode(path); });
        } else {
            connect(menu.addAction(tr("Reset to live")), &QAction::triggered,
                    this, [this, path] { _model->resetNode(path); });
        }
        menu.addSeparator();
    }
    connect(menu.addAction(tr("Reset All")), &QAction::triggered,
            this, &PropertyEditor::_onResetAll);
    menu.exec(_view->viewport()->mapToGlobal(pos));
}

} // namespace rpe
