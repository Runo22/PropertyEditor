#include "rpe/gui/PropertyEditor.h"

#include "rpe/core/TypeRenderer.h"
#include "rpe/gui/PropertyDelegate.h"
#include "rpe/gui/VariantEditorFactory.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPolygonF>
#include <QScopedValueRollback>
#include <QShortcut>
#include <QSortFilterProxyModel>
#include <QStyledItemDelegate>
#include <QTextDocument>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

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

        // The NAME column while a filter is active: the matched part of each name in
        // the highlight colour, and a group header's match count after its label.
        // Without a filter (and outside the grouped view) it paints exactly like the
        // default delegate.
        class NameDelegate : public QStyledItemDelegate
        {
        public:
            NameDelegate(std::function<QString()> needle, QObject* parent)
                : QStyledItemDelegate(parent)
                , _needle(std::move(needle))
            {
            }

            void paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const override
            {
                const QString needle = _needle();
                const bool group = index.data(IsGroupRole).toBool();
                if (needle.isEmpty() && !group)
                {
                    QStyledItemDelegate::paint(p, option, index);
                    return;
                }
                QStyleOptionViewItem o(option);
                initStyleOption(&o, index);
                const QString text = o.text;
                o.text.clear();
                const QWidget* w = o.widget;
                QStyle* style = w ? w->style() : QApplication::style();
                style->drawControl(QStyle::CE_ItemViewItem, &o, p, w);
                const QRect r = style->subElementRect(QStyle::SE_ItemViewItemText, &o, w).adjusted(2, 0, -2, 0);

                const bool selected = o.state & QStyle::State_Selected;
                const QColor base = o.palette.color(selected ? QPalette::HighlightedText : QPalette::Text);
                // On a selected row the highlight colour IS the background: mark the
                // match by weight there instead.
                const QColor mark = selected ? base : o.palette.color(QPalette::Highlight).lighter(150);
                QString html;
                const int at = needle.isEmpty() ? -1 : text.indexOf(needle, 0, Qt::CaseInsensitive);
                if (at >= 0)
                {
                    html = text.left(at).toHtmlEscaped()
                        + QStringLiteral("<b style='color:%1'>").arg(mark.name())
                        + text.mid(at, needle.size()).toHtmlEscaped() + QStringLiteral("</b>")
                        + text.mid(at + needle.size()).toHtmlEscaped();
                }
                else
                {
                    html = text.toHtmlEscaped();
                }
                if (group)
                {
                    html = QStringLiteral("<b>%1</b>").arg(html);
                    const int n = index.data(GroupCountRole).toInt();
                    if (n > 0)
                    {
                        const QColor muted = selected ? base : o.palette.color(QPalette::PlaceholderText);
                        html += QStringLiteral("&nbsp;&nbsp;<span style='color:%1'>&middot; %2</span>").arg(muted.name()).arg(n);
                    }
                }
                QTextDocument doc;
                doc.setDefaultFont(o.font);
                doc.setDocumentMargin(0);
                doc.setHtml(QStringLiteral("<span style='color:%1; white-space:pre'>%2</span>").arg(base.name(), html));
                p->save();
                p->setClipRect(r);
                p->translate(r.left(), r.top() + (r.height() - doc.size().height()) / 2.0);
                doc.drawContents(p);
                p->restore();
            }

        private:
            std::function<QString()> _needle;
        };

        // The scope button's glyph: three stacked layers.
        QIcon layersIcon(const QColor& c)
        {
            QPixmap pm(32, 32);
            pm.fill(Qt::transparent);
            QPainter p(&pm);
            p.setRenderHint(QPainter::Antialiasing);
            p.setPen(QPen(c, 2.2));
            p.setBrush(Qt::NoBrush);
            for (int i = 0; i < 3; ++i)
            {
                const double y = 9 + i * 6;
                p.drawPolygon(QPolygonF({ QPointF(16, y - 5), QPointF(27, y), QPointF(16, y + 5), QPointF(5, y) }));
            }
            return QIcon(pm);
        }
    } // namespace


    PropertyEditor::PropertyEditor(QWidget* parent)
        : QWidget(parent)
    {
        _model = new PropertyModel(this);
        _delegate = new PropertyDelegate(_model, this);
        _setupUi();
        connect(_model, &PropertyModel::propertyEdited, this, &PropertyEditor::propertyEdited);
        connect(_model, &PropertyModel::boundTypeReleased, this, [this](const QString& typeName) {
            _endScrub(false); // its value cell is gone
            emit boundTypeReleased(typeName);
        });
        connect(_model, &PropertyModel::localEditsChanged, this, &PropertyEditor::_updateResetEnabled);
        _updateResetEnabled(); // start hidden — nothing is frozen yet
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

        // Search scope: this object / all of them (the host decides what "all" is).
        _scopeBtn = new QToolButton(_toolbar);
        _scopeBtn->setCheckable(true);
        _scopeBtn->setToolTip(tr("Search all components of this entity (Ctrl+Shift+F)"));
        _scopeBtn->setStyleSheet(QStringLiteral(
            "QToolButton:checked { background: palette(highlight); color: palette(highlighted-text);"
            " border: 1px solid palette(highlight); border-radius: 3px; padding: 1px 6px; }"));
        _scopeBtn->setVisible(false);
        tb->addWidget(_scopeBtn);
        connect(_scopeBtn, &QToolButton::toggled, this, [this](bool on) {
            _scopeBtn->setIcon(layersIcon(on ? palette().color(QPalette::HighlightedText) : palette().color(QPalette::Text)));
            _scopeBtn->setText(on ? tr("All") : QString());
            _scopeBtn->setToolButtonStyle(on ? Qt::ToolButtonTextBesideIcon : Qt::ToolButtonIconOnly);
            _filter->setPlaceholderText(on ? tr("Search all components…") : tr("Filter properties…"));
            emit searchAllToggled(on);
        });
        _scopeBtn->setIcon(layersIcon(palette().color(QPalette::Text)));
        auto* scopeKey = new QShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F), this);
        scopeKey->setContext(Qt::WidgetWithChildrenShortcut);
        connect(scopeKey, &QShortcut::activated, this, [this] {
            if (!_searchAllAvailable)
            {
                return;
            }
            setSearchAll(!isSearchAll());
            _filter->setFocus();
            _filter->selectAll();
        });

        _resetBtn = new QToolButton(_toolbar);
        _resetBtn->setText(tr("Reset"));
        // Spell out what "Reset" does, since the short label can read like "reset to
        // defaults": it only releases FROZEN values and resumes live updates — it does
        // not change any value in the world.
        _resetBtn->setToolTip(tr("Release frozen values and resume live updates.\n"
                                 "Does not modify the world (only editing a value does)."));
        tb->addWidget(_resetBtn);

        root->addWidget(_toolbar);

        // Empty-state hint: a component can resolve (so it lists) yet reflect no
        // properties, which otherwise leaves a silent blank panel. See _updateEmptyHint.
        _emptyHint = new QLabel(this);
        _emptyHint->setWordWrap(true);
        _emptyHint->setContentsMargins(6, 4, 6, 4);
        _emptyHint->setStyleSheet(QStringLiteral("color: palette(mid);"));
        _emptyHint->setVisible(false);
        root->addWidget(_emptyHint);

        // proxy for filtering (matches property name OR value — see PropertyFilterProxy)
        _proxy = new PropertyFilterProxy(this);
        _proxy->setSourceModel(_model);
        _proxy->setRecursiveFilteringEnabled(true);

        _view = new QTreeView(this);
        _view->setModel(_proxy);
        _view->setItemDelegateForColumn(1, _delegate);
        _view->setItemDelegateForColumn(0, new NameDelegate([this] { return _filter->text().trimmed(); }, this));
        _view->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::SelectedClicked | QAbstractItemView::EditKeyPressed);
        // Drag-to-scrub watches the viewport (hover cursor, press/drag/release) and
        // the view (Esc mid-drag).
        _view->viewport()->setMouseTracking(true);
        _view->viewport()->installEventFilter(this);
        _view->installEventFilter(this);
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
        connect(_view->selectionModel(), &QItemSelectionModel::currentChanged, this, [this](const QModelIndex& cur) {
            emit currentPathChanged(cur.isValid() ? cur.siblingAtColumn(0).data(PropertyPathRole).toString() : QString());
        });
        // Grouped view: a header's count is what its rows show after filtering.
        connect(_proxy, &QAbstractItemModel::rowsInserted, this, &PropertyEditor::_scheduleGroupCounts);
        connect(_proxy, &QAbstractItemModel::rowsRemoved, this, &PropertyEditor::_scheduleGroupCounts);
        connect(_proxy, &QAbstractItemModel::modelReset, this, &PropertyEditor::_scheduleGroupCounts);
        connect(_proxy, &QAbstractItemModel::layoutChanged, this, &PropertyEditor::_scheduleGroupCounts);

        // Keep the model's notion of expansion in sync with the view, so collapsed
        // struct rows can show a "[a, b]" summary that disappears on expand.
        // (Custom roles pass through the proxy, so no mapToSource needed here.)
        // Outside the editor's own bulk operations (see _bulkExpanding), a per-row
        // expand/collapse is the user's choice — remembered per bound type.
        connect(_view, &QTreeView::expanded, this, [this](const QModelIndex& idx) {
            const QString path = idx.data(PropertyPathRole).toString();
            _model->setPathExpanded(path, true);
            if (!_bulkExpanding && !_boundTypeName.isEmpty())
            {
                _collapsedByType[_boundTypeName].remove(path);
            }
        });
        connect(_view, &QTreeView::collapsed, this, [this](const QModelIndex& idx) {
            const QString path = idx.data(PropertyPathRole).toString();
            _model->setPathExpanded(path, false);
            if (!_bulkExpanding && !_boundTypeName.isEmpty())
            {
                _collapsedByType[_boundTypeName].insert(path);
            }
        });
    }

    // Bulk expansion calls (expandAll / expandToDepth / collapseAll) do not
    // reliably emit the per-row expanded/collapsed signals (it varies across Qt 5
    // releases), so after any of them the model's expansion set is rebuilt by
    // walking the view.
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
        _boundTypeName = type.is_valid() ? QString::fromStdString(type.get_name().to_string()) : QString();
        const QScopedValueRollback<bool> bulk(_bulkExpanding, true);
        _model->bindType(type);
        _view->expandToDepth(0);
        _pushExpansionState();
        _updateResetEnabled(); // a fresh schema has no frozen nodes
        _updateEmptyHint(type);
    }

    void PropertyEditor::bindGroups(const QVector<PropertyModel::Group>& groups)
    {
        _boundTypeName.clear(); // no per-type expansion memory for a mixed view
        const QScopedValueRollback<bool> bulk(_bulkExpanding, true);
        _model->bindGroups(groups);
        _view->expandAll();
        _pushExpansionState();
        _updateResetEnabled();
        _emptyHint->setVisible(false);
        _scheduleGroupCounts();
    }

    void PropertyEditor::setGroupInstanceProvider(std::function<rttr::instance(const QString&)> provider)
    {
        _model->setGroupInstanceProvider(std::move(provider));
    }

    void PropertyEditor::_scheduleGroupCounts()
    {
        if (!_model->isGrouped() || _groupCountsPending)
        {
            return;
        }
        _groupCountsPending = true;
        QTimer::singleShot(0, this, [this] {
            _groupCountsPending = false;
            _updateGroupCounts();
        });
    }

    void PropertyEditor::_updateGroupCounts()
    {
        if (!_model->isGrouped())
        {
            return;
        }
        // A header's count = its visible rows that have no visible children (the
        // matches themselves, not the structs leading to them).
        QHash<QString, int> counts;
        for (int g = 0; g < _proxy->rowCount({}); ++g)
        {
            const QModelIndex header = _proxy->index(g, 0, {});
            int n = 0;
            QList<QModelIndex> stack;
            for (int r = 0; r < _proxy->rowCount(header); ++r)
                stack.append(_proxy->index(r, 0, header));
            while (!stack.isEmpty())
            {
                const QModelIndex i = stack.takeLast();
                const int rows = _proxy->rowCount(i);
                if (rows == 0)
                {
                    ++n;
                }
                for (int r = 0; r < rows; ++r)
                    stack.append(_proxy->index(r, 0, i));
            }
            counts.insert(header.data(GroupKeyRole).toString(), _filter->text().trimmed().isEmpty() ? 0 : n);
        }
        _model->setGroupCounts(counts);
    }

    bool PropertyEditor::revealPath(const QString& path)
    {
        const QModelIndex src = _model->indexForPath(path);
        const QModelIndex idx = src.isValid() ? _proxy->mapFromSource(src) : QModelIndex();
        if (!idx.isValid())
        {
            return false;
        }
        for (QModelIndex p = idx.parent(); p.isValid(); p = p.parent())
        {
            _view->expand(p);
        }
        _view->scrollTo(idx, QAbstractItemView::PositionAtCenter);
        _view->setCurrentIndex(idx);
        return true;
    }

    void PropertyEditor::revealGroup(const QString& key)
    {
        const QModelIndex src = _model->groupIndex(key);
        const QModelIndex idx = src.isValid() ? _proxy->mapFromSource(src) : QModelIndex();
        if (idx.isValid())
        {
            _view->expand(idx);
            _view->scrollTo(idx, QAbstractItemView::PositionAtTop);
            _view->setCurrentIndex(idx);
        }
    }

    // ── search scope ──────────────────────────────────────────────────────────────

    void PropertyEditor::setSearchAllAvailable(bool on)
    {
        _searchAllAvailable = on;
        _scopeBtn->setVisible(on);
        if (!on)
        {
            setSearchAll(false);
        }
    }

    void PropertyEditor::setSearchAll(bool on)
    {
        _scopeBtn->setChecked(on && _searchAllAvailable);
    }

    bool PropertyEditor::isSearchAll() const
    {
        return _scopeBtn->isChecked();
    }

    QString PropertyEditor::filterText() const
    {
        return _filter->text().trimmed();
    }

    void PropertyEditor::setFilterText(const QString& text)
    {
        _filter->setText(text);
    }

    void PropertyEditor::unbind()
    {
        _model->unbind();
        _updateResetEnabled();
        _emptyHint->setVisible(false);
    }

    // A type can be perfectly valid yet reflect NOTHING: rttr::type::get<T>() succeeds
    // for any complete type, registration or not. Such a component still resolves and
    // still lists — it just binds an empty schema, so the panel goes blank with no
    // explanation. Say so, and point at the usual cause (a registration block the
    // linker dropped, which is why it typically only bites in Release).
    void PropertyEditor::_updateEmptyHint(rttr::type t)
    {
        const bool noProps = t.is_valid() && t.get_properties().empty();
        if (noProps)
        {
            const auto n = t.get_name();
            _emptyHint->setText(tr("“%1” reflects no properties — nothing to edit.")
                                    .arg(QString::fromUtf8(n.data(), static_cast<int>(n.size()))));
            _emptyHint->setToolTip(tr(
                "The type resolved, but RTTR reports no properties for it.\n\n"
                "If this works in a Debug build but not in Release, its RTTR_REGISTRATION "
                "block was most likely stripped by the linker as an unreferenced static "
                "initializer. Force-link that translation unit (e.g. --whole-archive / "
                "/WHOLEARCHIVE, or reference a symbol from it)."));
        }
        _emptyHint->setVisible(noProps);
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
        _updateResetEnabled();
    }

    // The Reset button releases frozen (locally-edited) values — enable it only while
    // the current component actually has one, so it reads as an active affordance
    // rather than an always-lit button that looks like "reset to defaults".
    void PropertyEditor::_updateResetEnabled()
    {
        // Shown only while there is something to release: frozen values are rare
        // (LocalEdit drafts, "Local edit" from the context menu), so a permanently
        // visible — and almost always disabled — button was just clutter.
        const bool needed = !isReadOnly() && _model->hasAnyLocalEdit();
        _resetBtn->setEnabled(needed);
        _resetBtn->setVisible(needed);
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
    // ── Drag-to-scrub ─────────────────────────────────────────────────────────────

    void PropertyEditor::setDragToScrubEnabled(bool on)
    {
        _scrubEnabled = on;
        if (!on)
        {
            _endScrub(false);
            _view->viewport()->unsetCursor();
        }
    }

    bool PropertyEditor::_isScrubbable(const QModelIndex& nameIndex) const
    {
        if (!nameIndex.isValid() || nameIndex.column() != 0)
        {
            return false;
        }
        const QModelIndex value = nameIndex.siblingAtColumn(1);
        if (!(value.flags() & Qt::ItemIsEditable) || !value.data(IsLeafRole).toBool())
        {
            return false; // read-only rows, structs, the whole editor in read-only mode
        }
        const rttr::variant declared = value.data(DeclaredTypeRole).value<rttr::variant>();
        if (!declared.is_valid())
        {
            return false;
        }
        const rttr::type t = TypeRenderer::rawType(declared.get_value<rttr::type>());
        return t.is_arithmetic() && t != rttr::type::get<bool>();
    }

    void PropertyEditor::_applyScrub(int dx, Qt::KeyboardModifiers mods)
    {
        if (!_scrubValue.isValid())
        {
            return;
        }
        const rttr::type t = TypeRenderer::rawType(
            _scrubValue.data(DeclaredTypeRole).value<rttr::variant>().get_value<rttr::type>());
        const bool floating = t == rttr::type::get<float>() || t == rttr::type::get<double>()
            || t == rttr::type::get<long double>();
        const QVariant stepHint = _scrubValue.data(StepRole);
        double step = stepHint.isValid() && stepHint.toDouble() > 0 ? stepHint.toDouble() : (floating ? 0.1 : 1.0);
        if (mods & Qt::ShiftModifier)
        {
            step *= 0.1; // fine
        }
        else if (mods & Qt::ControlModifier)
        {
            step *= 10.0; // coarse
        }
        constexpr double kPixelsPerStep = 4.0;
        double v = _scrubStart + (dx / kPixelsPerStep) * step;
        if (!floating)
        {
            v = std::round(v);
        }
        if (const QVariant lo = _scrubValue.data(MinRole); lo.isValid())
        {
            v = std::max(v, lo.toDouble());
        }
        if (const QVariant hi = _scrubValue.data(MaxRole); hi.isValid())
        {
            v = std::min(v, hi.toDouble());
        }
        if (v == _scrubLast)
        {
            return; // nothing changed — no write
        }
        const rttr::variant typed = varedit::numberAs(v, t);
        if (typed.is_valid() && _view->model()->setData(_scrubValue, QVariant::fromValue(typed), Qt::EditRole))
        {
            _scrubLast = v;
        }
    }

    void PropertyEditor::_endScrub(bool restore)
    {
        if (_scrubbing && restore && _scrubValue.isValid() && _scrubLast != _scrubStart)
        {
            const rttr::type t = TypeRenderer::rawType(
                _scrubValue.data(DeclaredTypeRole).value<rttr::variant>().get_value<rttr::type>());
            const rttr::variant typed = varedit::numberAs(_scrubStart, t);
            if (typed.is_valid())
            {
                _view->model()->setData(_scrubValue, QVariant::fromValue(typed), Qt::EditRole);
            }
        }
        _scrubArmed = false;
        _scrubbing = false;
        _scrubValue = QPersistentModelIndex();
    }

    bool PropertyEditor::eventFilter(QObject* obj, QEvent* ev)
    {
        if (obj == _view->viewport() && _scrubEnabled)
        {
            switch (ev->type())
            {
            case QEvent::MouseMove:
            {
                auto* me = static_cast<QMouseEvent*>(ev);
                if (_scrubArmed && (me->buttons() & Qt::LeftButton))
                {
                    const int dx = me->pos().x() - _scrubPressX;
                    if (!_scrubbing)
                    {
                        if (std::abs(dx) < QApplication::startDragDistance())
                        {
                            return false; // still a click, not a drag
                        }
                        // Crossing the threshold STARTS the drag here: re-anchor, so
                        // the value doesn't jump by the threshold's worth of pixels.
                        _scrubbing = true;
                        _scrubPressX = me->pos().x();
                        return true;
                    }
                    _applyScrub(dx, me->modifiers());
                    return true; // no rubber-band / drag-selection while scrubbing
                }
                // Hover: advertise the scrubbable names.
                if (_isScrubbable(_view->indexAt(me->pos())))
                {
                    _view->viewport()->setCursor(Qt::SizeHorCursor);
                }
                else
                {
                    _view->viewport()->unsetCursor();
                }
                return false;
            }
            case QEvent::MouseButtonPress:
            {
                auto* me = static_cast<QMouseEvent*>(ev);
                const QModelIndex idx = _view->indexAt(me->pos());
                if (me->button() == Qt::LeftButton && _isScrubbable(idx))
                {
                    // Arm, but let the press through: a plain click still selects.
                    _scrubArmed = true;
                    _scrubbing = false;
                    _scrubPressX = me->pos().x();
                    _scrubValue = idx.siblingAtColumn(1);
                    const rttr::variant v = TypeRenderer::unwrap(_scrubValue.data(RttrVariantRole).value<rttr::variant>());
                    _scrubStart = _scrubLast = v.is_valid() ? v.to_double() : 0.0;
                }
                return false;
            }
            case QEvent::MouseButtonRelease:
            {
                const bool wasScrubbing = _scrubbing;
                _endScrub(false);
                return wasScrubbing; // a drag's release isn't a click
            }
            default:
                break;
            }
        }
        if (obj == _view && ev->type() == QEvent::KeyPress && _scrubbing
            && static_cast<QKeyEvent*>(ev)->key() == Qt::Key_Escape)
        {
            _endScrub(true); // Esc mid-drag: back to where it started
            return true;
        }
        return QWidget::eventFilter(obj, ev);
    }

    void PropertyEditor::expandAll()
    {
        const QScopedValueRollback<bool> bulk(_bulkExpanding, true);
        _view->expandAll();
        _pushExpansionState();
    }

    void PropertyEditor::expandAllExceptCollapsed()
    {
        const QSet<QString> keep = _collapsedByType.value(_boundTypeName);
        const QScopedValueRollback<bool> bulk(_bulkExpanding, true);
        _view->expandAll();
        if (!keep.isEmpty())
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
                if (keep.contains(idx.data(PropertyPathRole).toString()))
                {
                    _view->collapse(idx);
                    continue; // nothing below a collapsed row is visible anyway
                }
                for (int r = rows - 1; r >= 0; --r)
                {
                    stack.append(_proxy->index(r, 0, idx));
                }
            }
        }
        _pushExpansionState();
    }

    // ── slots ─────────────────────────────────────────────────────────────────────

    void PropertyEditor::_onFilterChanged(const QString& text)
    {
        const QScopedValueRollback<bool> bulk(_bulkExpanding, true);
        static_cast<PropertyFilterProxy*>(_proxy)->setFilterText(text);
        _view->viewport()->update(); // the name column highlights the new text
        if (text.isEmpty())
        {
            _view->expandToDepth(0);
        }
        else
        {
            _view->expandAll();
        }
        _pushExpansionState();
        _scheduleGroupCounts();
        emit filterTextChanged(text.trimmed());
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
