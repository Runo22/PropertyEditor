#include "rttr_property_editor/widgets/PropertyEditor.h"
#include "rttr_property_editor/core/PropertyDelegate.h"
#include "rttr_property_editor/core/PropertyModel.h"

#include <QAction>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QTreeView>
#include <QVBoxLayout>

namespace rpe {


// ── PropertyEditor ────────────────────────────────────────────────────────────

PropertyEditor::PropertyEditor(QWidget* parent)
    : QWidget(parent)
{
    _model    = new PropertyModel(this);
    _delegate = new PropertyDelegate(_model, this);
    _setupUi();

    connect(_model, &PropertyModel::propertyEdited,
            this,   &PropertyEditor::propertyEdited);
}

void PropertyEditor::_setupUi()
{
    auto* vLayout = new QVBoxLayout(this);
    vLayout->setContentsMargins(0, 0, 0, 0);
    vLayout->setSpacing(2);

    // ── Toolbar ───────────────────────────────────────────────────────────────
    auto* toolbar = new QWidget(this);
    auto* hLayout = new QHBoxLayout(toolbar);
    hLayout->setContentsMargins(4, 2, 4, 2);
    hLayout->setSpacing(4);

    _filterEdit = new QLineEdit(toolbar);
    _filterEdit->setPlaceholderText(QStringLiteral("Filter properties..."));
    _filterEdit->setClearButtonEnabled(true);
    hLayout->addWidget(_filterEdit, 1);

    _modeCombo = new QComboBox(toolbar);
    _modeCombo->addItem(QStringLiteral("Edit"), static_cast<int>(DisplayMode::EditLive));
    _modeCombo->addItem(QStringLiteral("Read Only"), static_cast<int>(DisplayMode::ReadOnly));
    hLayout->addWidget(_modeCombo);

    _resetAllBtn = new QPushButton(QStringLiteral("Reset All"), toolbar);
    _resetAllBtn->setToolTip(QStringLiteral("Release all overridden values"));
    hLayout->addWidget(_resetAllBtn);

    vLayout->addWidget(toolbar);

    // ── Tree view ─────────────────────────────────────────────────────────────
    auto* proxy = new QSortFilterProxyModel(this);
    proxy->setSourceModel(_model);
    proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
    proxy->setFilterKeyColumn(0);              // filter on property name column
    proxy->setRecursiveFilteringEnabled(true); // Qt 5.10+: show parent when child matches

    _view = new QTreeView(this);
    _view->setModel(proxy);
    _view->setItemDelegateForColumn(1, _delegate);
    _view->setEditTriggers(QAbstractItemView::DoubleClicked |
                           QAbstractItemView::SelectedClicked);
    _view->setAlternatingRowColors(true);
    _view->setUniformRowHeights(true);
    _view->setAnimated(false); // better perf with frequent updates
    _view->header()->setStretchLastSection(true);
    _view->header()->resizeSection(0, 200);
    _view->setContextMenuPolicy(Qt::CustomContextMenu);

    vLayout->addWidget(_view, 1);

    // ── Connections ───────────────────────────────────────────────────────────
    connect(_filterEdit,  &QLineEdit::textChanged,
            this,         &PropertyEditor::_onFilterChanged);
    connect(_modeCombo,   qOverload<int>(&QComboBox::currentIndexChanged),
            this,         &PropertyEditor::_onModeChanged);
    connect(_resetAllBtn, &QPushButton::clicked,
            this,         &PropertyEditor::_onResetAll);
    connect(_view,        &QTreeView::customContextMenuRequested,
            this,         &PropertyEditor::_onContextMenu);
}

// ── Public API ────────────────────────────────────────────────────────────────

void PropertyEditor::bindType(rttr::type type)
{
    _model->bindType(type);
    _view->expandAll();
}

void PropertyEditor::unbind()
{
    _model->unbind();
}

void PropertyEditor::refresh(const rttr::instance& obj)
{
    _model->refresh(obj);
}

void PropertyEditor::setPropertyValue(const QString& path, rttr::variant val)
{
    _model->setPropertyValue(path, std::move(val));
}

void PropertyEditor::setMode(DisplayMode mode)
{
    _mode = mode;
    const bool editable = (mode == DisplayMode::EditLive);
    _modeCombo->setCurrentIndex(editable ? 0 : 1);
    _resetAllBtn->setEnabled(editable);
    // The model flags() drives editability; expose readOnly via a flag
    // (PropertyModel doesn't have _readOnly exposed yet — set indirectly via view)
    _view->setEditTriggers(editable
        ? QAbstractItemView::DoubleClicked | QAbstractItemView::SelectedClicked
        : QAbstractItemView::NoEditTriggers);
}

// ── Slots ─────────────────────────────────────────────────────────────────────

void PropertyEditor::_onFilterChanged(const QString& text)
{
    auto* proxy = qobject_cast<QSortFilterProxyModel*>(_view->model());
    if (!proxy) return;
    proxy->setFilterFixedString(text);
    if (text.isEmpty())
        _view->collapseAll();
    else
        _view->expandAll();
}

void PropertyEditor::_onModeChanged(int index)
{
    const auto mode = static_cast<DisplayMode>(
        _modeCombo->itemData(index).toInt());
    if (mode != _mode)
        setMode(mode);
}

void PropertyEditor::_onResetAll()
{
    _model->resetAll();
}

void PropertyEditor::_onContextMenu(const QPoint& pos)
{
    if (_mode == DisplayMode::ReadOnly) return;

    const QModelIndex proxyIdx = _view->indexAt(pos);
    if (!proxyIdx.isValid()) return;

    auto* proxy = qobject_cast<QSortFilterProxyModel*>(_view->model());
    const QModelIndex srcIdx = proxy ? proxy->mapToSource(proxyIdx) : proxyIdx;
    if (!srcIdx.isValid()) return;

    const QString path       = srcIdx.data(PropertyPathRole).toString();
    const bool    overridden = srcIdx.data(IsOverriddenRole).toBool();

    QMenu menu(this);
    if (!overridden) {
        auto* actOverride = menu.addAction(QStringLiteral("Override (freeze value)"));
        connect(actOverride, &QAction::triggered, this, [this, path] {
            _model->overrideNode(path);
        });
    } else {
        auto* actReset = menu.addAction(QStringLiteral("Reset (resume live updates)"));
        connect(actReset, &QAction::triggered, this, [this, path] {
            _model->resetNode(path);
        });
    }
    menu.addSeparator();
    auto* actResetAll = menu.addAction(QStringLiteral("Reset All Overrides"));
    connect(actResetAll, &QAction::triggered, this, &PropertyEditor::_onResetAll);

    menu.exec(_view->viewport()->mapToGlobal(pos));
}

} // namespace rpe
