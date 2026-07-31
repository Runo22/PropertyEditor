#include "rpe/gui/PropertyEditor.h"

#include "rpe/gui/PropertyDelegate.h"

#include <QAction>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLineEdit>
#include <QApplication>
#include <QClipboard>
#include <QMenu>
#include <QSortFilterProxyModel>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>

namespace rpe
{
    namespace
    {
        // Filters property rows by NAME (column 0 display) OR VALUE (the model's
        // expansion-independent FilterValueRole), so typing "7.5" or a struct's
        // "[1, 2]" summary narrows the tree too — not just names. Recursive: a row
        // survives if it matches or any descendant does (ancestors of a match stay
        // visible). Only runs on filter-text change, so the extra per-row value read
        // (a cached string) costs nothing at steady state.
        class PropertyFilterProxy : public QSortFilterProxyModel
        {
        public:
            using QSortFilterProxyModel::QSortFilterProxyModel;
            void setFilterText(const QString& t)
            {
                _text = t.trimmed();
                invalidateFilter();
            }

        protected:
            bool filterAcceptsRow(int row, const QModelIndex& parent) const override
            {
                if (_text.isEmpty())
                {
                    return true;
                }
                const QModelIndex idx = sourceModel()->index(row, 0, parent);
                if (idx.data(Qt::DisplayRole).toString().contains(_text, Qt::CaseInsensitive))
                {
                    return true;
                }
                return idx.data(FilterValueRole).toString().contains(_text, Qt::CaseInsensitive);
            }

        private:
            QString _text;
        };
    } // namespace


    PropertyEditor::PropertyEditor(QWidget* parent)
        : QWidget(parent)
    {
        _model = new PropertyModel(this);
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
        _resetBtn->setToolTip(tr("Release all local edits (return to live values)"));
        tb->addWidget(_resetBtn);

        root->addWidget(_toolbar);

        // proxy for filtering (matches property name OR value — see PropertyFilterProxy)
        _proxy = new PropertyFilterProxy(this);
        _proxy->setSourceModel(_model);
        _proxy->setRecursiveFilteringEnabled(true);

        _view = new QTreeView(this);
        _view->setModel(_proxy);
        _view->setItemDelegateForColumn(1, _delegate);
        _view->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::SelectedClicked | QAbstractItemView::EditKeyPressed);
        _view->setAlternatingRowColors(true);
        _view->setUniformRowHeights(true);
        _view->setAnimated(false); // keep redraws cheap under live updates
        _view->setExpandsOnDoubleClick(false);
        _view->header()->setStretchLastSection(true);
        _view->header()->resizeSection(0, 200);
        _view->setContextMenuPolicy(Qt::CustomContextMenu);
        root->addWidget(_view, 1);

        connect(_filter, &QLineEdit::textChanged, this, &PropertyEditor::_onFilterChanged);
        connect(_resetBtn, &QToolButton::clicked, this, &PropertyEditor::_onResetAll);
        connect(_view, &QTreeView::customContextMenuRequested, this, &PropertyEditor::_onContextMenu);

        // Keep the model's notion of expansion in sync with the view, so collapsed
        // struct rows can show a "[a, b]" summary that disappears on expand.
        // (Custom roles pass through the proxy, so no mapToSource needed here.)
        connect(_view, &QTreeView::expanded, this, [this](const QModelIndex& idx) {
            _model->setPathExpanded(idx.data(PropertyPathRole).toString(), true);
        });
        connect(_view, &QTreeView::collapsed, this, [this](const QModelIndex& idx) {
            _model->setPathExpanded(idx.data(PropertyPathRole).toString(), false);
        });
    }

    // Bulk expansion calls (expandAll / expandToDepth / collapseAll) do NOT emit
    // the per-row expanded/collapsed signals, so after any of them the model's
    // expansion set must be rebuilt by walking the view.
    void PropertyEditor::_pushExpansionState()
    {
        QList<QModelIndex> stack;
        for (int r = _proxy->rowCount({}) - 1; r >= 0; --r)
        {
            stack.append(_proxy->index(r, 0, {}));
        }
        while (!stack.isEmpty())
        {
            const QModelIndex idx = stack.takeLast();
            const int rows = _proxy->rowCount(idx);
            if (rows == 0)
            {
                continue;
            }
            _model->setPathExpanded(idx.data(PropertyPathRole).toString(), _view->isExpanded(idx));
            for (int r = rows - 1; r >= 0; --r)
            {
                stack.append(_proxy->index(r, 0, idx));
            }
        }
    }

    // ── data / schema ────────────────────────────────────────────────────────────

    void PropertyEditor::bindType(rttr::type type)
    {
        _model->bindType(type);
        _view->expandToDepth(0);
        _pushExpansionState();
    }

    void PropertyEditor::unbind()
    {
        _model->unbind();
    }
    void PropertyEditor::refresh(const rttr::instance& obj)
    {
        _model->refresh(obj);
    }
    void PropertyEditor::setPropertyValue(const QString& p, rttr::variant v)
    {
        _model->setPropertyValue(p, std::move(v));
    }

    // ── behaviour ─────────────────────────────────────────────────────────────────

    void PropertyEditor::setReadOnly(bool ro)
    {
        _model->setReadOnly(ro);
        _resetBtn->setEnabled(!ro);
    }
    bool PropertyEditor::isReadOnly() const
    {
        return _model->isReadOnly();
    }

    void PropertyEditor::setEditPolicy(EditPolicy p)
    {
        _model->setEditPolicy(p);
    }
    EditPolicy PropertyEditor::editPolicy() const
    {
        return _model->editPolicy();
    }

    void PropertyEditor::setInstanceProvider(std::function<rttr::instance()> provider)
    {
        _model->setInstanceProvider(std::move(provider));
    }

    void PropertyEditor::setWriteGuard(AccessGuard guard)
    {
        _model->setWriteGuard(std::move(guard));
    }

    void PropertyEditor::setEditSink(std::function<void(const QString&, const rttr::variant&)> sink)
    {
        _model->setEditSink(std::move(sink));
    }

    void PropertyEditor::setPinnedPaths(const QSet<QString>& paths)
    {
        _model->setPinnedPaths(paths);
    }

    QStringList PropertyEditor::visibleLeafPaths(bool onlyExpanded) const
    {
        if (!onlyExpanded)
        {
            return _model->allLeafPaths();
        }

        // Walk the proxy tree, descending only into expanded rows, collecting leaves.
        QStringList out;
        QList<QModelIndex> stack;
        const int topRows = _proxy->rowCount({});
        for (int r = topRows - 1; r >= 0; --r)
        {
            stack.append(_proxy->index(r, 0, {}));
        }

        while (!stack.isEmpty())
        {
            const QModelIndex idx = stack.takeLast();
            const int rows = _proxy->rowCount(idx);
            if (rows == 0)
            {
                out.append(idx.data(PropertyPathRole).toString());
            }
            else if (_view->isExpanded(idx))
            {
                // An expanded array is watched at its OWN path too (not just its
                // elements): the whole-vector read is what lets the mirror notice a
                // resize — element paths alone can't (a grown index isn't watched, a
                // shrunk one reads out of range). Element values still update via
                // their own paths below.
                if (idx.data(IsArrayRole).toBool())
                {
                    out.append(idx.data(PropertyPathRole).toString());
                }
                for (int r = rows - 1; r >= 0; --r)
                {
                    stack.append(_proxy->index(r, 0, idx));
                }
            }
            else if (!idx.data(IsArrayRole).toBool() && rows <= 4)
            {
                // A collapsed small struct shows a "[a, b]" summary of its direct
                // leaf children — those leaves must be watched even though their
                // rows are hidden, or the summary would stay forever blank in
                // mirror mode. Nested structs inside it render as "…" and need no
                // watch.
                for (int r = 0; r < rows; ++r)
                {
                    const QModelIndex c = _proxy->index(r, 0, idx);
                    if (_proxy->rowCount(c) == 0)
                    {
                        out.append(c.data(PropertyPathRole).toString());
                    }
                }
            }
        }
        return out;
    }

    // ── chrome ──────────────────────────────────────────────────────────────────

    void PropertyEditor::setToolbarVisible(bool visible)
    {
        _toolbar->setVisible(visible);
    }
    void PropertyEditor::expandAll()
    {
        _view->expandAll();
        _pushExpansionState();
    }

    // ── slots ─────────────────────────────────────────────────────────────────────

    void PropertyEditor::_onFilterChanged(const QString& text)
    {
        static_cast<PropertyFilterProxy*>(_proxy)->setFilterText(text);
        if (text.isEmpty())
        {
            _view->expandToDepth(0);
        }
        else
        {
            _view->expandAll();
        }
        _pushExpansionState();
    }

    void PropertyEditor::_onResetAll()
    {
        _model->resetAll();
    }

    void PropertyEditor::_onContextMenu(const QPoint& pos)
    {
        const QModelIndex proxyIdx = _view->indexAt(pos);
        if (!proxyIdx.isValid())
        {
            return;
        }
        const QModelIndex srcIdx = _proxy->mapToSource(proxyIdx);
        if (!srcIdx.isValid())
        {
            return;
        }

        const QString path = srcIdx.data(PropertyPathRole).toString();
        const bool hasLocalEdit = srcIdx.data(HasLocalEditRole).toBool();
        const QString name = srcIdx.siblingAtColumn(0).data(Qt::DisplayRole).toString();
        const QString value = srcIdx.siblingAtColumn(1).data(Qt::DisplayRole).toString();

        QMenu menu(this);
        // Copy the row's text to the clipboard — available regardless of read-only /
        // pinning, since it never mutates anything.
        connect(menu.addAction(tr("Copy value")), &QAction::triggered, this,
                [value] { QApplication::clipboard()->setText(value); });
        connect(menu.addAction(tr("Copy name")), &QAction::triggered, this,
                [name] { QApplication::clipboard()->setText(name); });
        connect(menu.addAction(tr("Copy \"name = value\"")), &QAction::triggered, this,
                [name, value] { QApplication::clipboard()->setText(name + QStringLiteral(" = ") + value); });
        menu.addSeparator();
        if (!isReadOnly())
        {
            if (!hasLocalEdit)
            {
                connect(menu.addAction(tr("Local edit (freeze live value)")), &QAction::triggered, this, [this, path] { _model->beginLocalEdit(path); });
            }
            else
            {
                connect(menu.addAction(tr("Reset to live")), &QAction::triggered, this, [this, path] { _model->resetNode(path); });
            }
            menu.addSeparator();
        }
        if (_pinningEnabled && srcIdx.data(IsLeafRole).toBool())
        {
            if (!_model->isPinnedPath(path))
            {
                connect(menu.addAction(tr("Pin to watch list")), &QAction::triggered, this, [this, path] { emit pinRequested(path); });
            }
            else
            {
                connect(menu.addAction(tr("Unpin from watch list")), &QAction::triggered, this, [this, path] { emit unpinRequested(path); });
            }
            menu.addSeparator();
        }
        connect(menu.addAction(tr("Reset All")), &QAction::triggered, this, &PropertyEditor::_onResetAll);
        menu.exec(_view->viewport()->mapToGlobal(pos));
    }

} // namespace rpe
