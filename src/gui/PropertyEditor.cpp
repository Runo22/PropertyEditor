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
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <limits>

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

        // The NAME column. The style draws every row's text as usual — its colours
        // (style-sheet ::item rules included), its font — and while a filter is
        // active the matched part gets a translucent accent tint over it, never a
        // re-coloured or re-drawn text. Group headers (the grouped view) are the
        // view's own font in bold, with their match count appended.
        class NameDelegate : public QStyledItemDelegate
        {
        public:
            NameDelegate(std::function<QString()> needle, QObject* parent)
                : QStyledItemDelegate(parent)
                , _needle(std::move(needle))
            {
            }

        protected:
            void initStyleOption(QStyleOptionViewItem* o, const QModelIndex& index) const override
            {
                QStyledItemDelegate::initStyleOption(o, index);
                if (index.data(IsGroupRole).toBool())
                {
                    o->font.setBold(true); // the view's font, bold — not the app default
                    o->fontMetrics = QFontMetrics(o->font);
                    const int n = index.data(GroupCountRole).toInt();
                    if (n > 0)
                    {
                        o->text += QStringLiteral("  \u00B7  %1").arg(n);
                    }
                }
            }

        public:
            void paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const override
            {
                const QString needle = _needle();
                QStyleOptionViewItem o(option);
                initStyleOption(&o, index);
                const int at = needle.isEmpty() ? -1 : o.text.indexOf(needle, 0, Qt::CaseInsensitive);
                if (at < 0)
                {
                    QStyledItemDelegate::paint(p, option, index);
                    return;
                }
                const QWidget* w = o.widget;
                QStyle* style = w ? w->style() : QApplication::style();
                // Where the match sits in the text the style is about to draw (left
                // aligned after the style's text margin, possibly elided).
                const QRect textRect = style->subElementRect(QStyle::SE_ItemViewItemText, &o, w);
                const int margin = style->pixelMetric(QStyle::PM_FocusFrameHMargin, nullptr, w) + 1;
                const QFontMetrics fm(o.font);
                const QString shown = fm.elidedText(o.text, o.textElideMode, textRect.width() - 2 * margin);
                QRect hl;
                if (shown == o.text || at + needle.size() < shown.size())
                {
                    const int x = textRect.left() + margin + fm.horizontalAdvance(o.text.left(at));
                    const int width = fm.horizontalAdvance(o.text.mid(at, needle.size()));
                    hl = QRect(x - 1, textRect.center().y() - fm.height() / 2, width + 2, fm.height()) & textRect;
                }
                // The ACTIVE accent, whatever has focus: typing in the filter leaves
                // the tree inactive, and an inactive highlight is grey on Windows. On
                // a selected row, the colour the style draws selected text in.
                // Painted OVER the item, translucent: anything the style (or a style
                // sheet's ::item background) paints under the text would cover a tint
                // drawn first.
                const bool selected = o.state & QStyle::State_Selected;
                QColor tint = selected ? selectedTextColor(o) : o.palette.color(QPalette::Active, QPalette::Highlight);
                tint.setAlpha(selected ? 70 : 95);
                QStyledItemDelegate::paint(p, option, index);
                if (!hl.isEmpty())
                {
                    p->save();
                    p->setRenderHint(QPainter::Antialiasing, true);
                    p->setPen(Qt::NoPen);
                    p->setBrush(tint);
                    p->drawRoundedRect(hl, 2, 2);
                    p->restore();
                }
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
        // Icon + "All" in both states — the button doesn't change width when toggled
        // — and no Tab stop of its own (Ctrl+Shift+F is the keyboard way).
        _scopeBtn->setText(tr("All"));
        _scopeBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        _scopeBtn->setFocusPolicy(Qt::NoFocus);
        _scopeBtn->setToolTip(tr("Search all components of this entity (Ctrl+Shift+F)"));
        // Same box in both states (only the colours change), so toggling never moves
        // the filter box.
        _scopeBtn->setStyleSheet(QStringLiteral(
            "QToolButton { border: 1px solid transparent; border-radius: 3px; padding: 1px 6px; }"
            " QToolButton:hover { border-color: palette(mid); }"
            " QToolButton:checked { background: palette(highlight); color: palette(highlighted-text);"
            " border-color: palette(highlight); }"));
        _scopeBtn->setVisible(false);
        tb->addWidget(_scopeBtn);
        // The glyph is drawn in the button's text colour — known only once the
        // style sheet has been applied (polish), and again on any theme change.
        _scopeBtn->installEventFilter(this);
        connect(_scopeBtn, &QToolButton::toggled, this, [this](bool on) {
            _updateScopeIcon();
            _filter->setPlaceholderText(on ? tr("Search all components…") : tr("Filter properties…"));
            emit searchAllToggled(on);
        });
        _updateScopeIcon();
        // Disabled until setSearchAllAvailable(true): an ENABLED shortcut on the same
        // keys as a host action makes Qt treat both as ambiguous, and neither fires.
        _scopeKey = new QShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F), this);
        _scopeKey->setContext(Qt::WidgetWithChildrenShortcut);
        _scopeKey->setEnabled(false);
        connect(_scopeKey, &QShortcut::activated, this, [this] {
            if (!_searchAllAvailable)
            {
                return;
            }
            setSearchAll(!isSearchAll());
            // Caret at the end, text NOT selected — like Tab back into a filter box.
            // Selected text is drawn in the palette's HighlightedText, which many
            // dark palettes leave black: the filter text looked like it turned black.
            _filter->setFocus(Qt::ShortcutFocusReason);
            _filter->deselect();
            _filter->end(false);
        });

        _resetBtn = new QToolButton(_toolbar);
        _resetBtn->setText(tr("Reset"));
        // Shown only while something is frozen — but its room is kept, so the
        // filter box doesn't change width every time a value freezes or resumes.
        {
            QSizePolicy sp = _resetBtn->sizePolicy();
            sp.setRetainSizeWhenHidden(true);
            _resetBtn->setSizePolicy(sp);
        }
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
    }

    void PropertyEditor::setGroupInstanceProvider(std::function<rttr::instance(const QString&)> provider)
    {
        _model->setGroupInstanceProvider(std::move(provider));
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

    void PropertyEditor::setSearchAllShortcut(const QKeySequence& keys)
    {
        _scopeKey->setKey(keys);
        _scopeKey->setEnabled(_searchAllAvailable && !keys.isEmpty());
        _scopeBtn->setToolTip(keys.isEmpty() ? tr("Search all components of this entity")
                                             : tr("Search all components of this entity (%1)")
                                                   .arg(keys.toString(QKeySequence::NativeText)));
    }

    QKeySequence PropertyEditor::searchAllShortcut() const
    {
        return _scopeKey->key();
    }

    void PropertyEditor::setSearchAllAvailable(bool on)
    {
        _searchAllAvailable = on;
        _scopeKey->setEnabled(on && !_scopeKey->key().isEmpty());
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
        if (!_filter->text().trimmed().isEmpty())
        {
            // While filtering, a row can be hidden only because its value hasn't
            // arrived yet (it may match by VALUE). Watching just the shown rows
            // would never fetch it, and it would stay hidden: watch every leaf —
            // and every array, whose elements exist only once its value is read.
            QStringList out;
            QList<QModelIndex> stack { QModelIndex() };
            while (!stack.isEmpty())
            {
                const QModelIndex parent = stack.takeLast();
                for (int r = 0; r < _model->rowCount(parent); ++r)
                {
                    const QModelIndex idx = _model->index(r, 0, parent);
                    if (idx.data(IsArrayRole).toBool())
                    {
                        out.append(idx.data(PropertyPathRole).toString());
                    }
                    if (_model->rowCount(idx) == 0)
                    {
                        if (!idx.data(IsArrayRole).toBool())
                            out.append(idx.data(PropertyPathRole).toString());
                    }
                    else
                    {
                        stack.append(idx);
                    }
                }
            }
            return out;
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

    namespace
    {
        // The representable range of an arithmetic type (false for others).
        bool numericRange(rttr::type t, double& lo, double& hi)
        {
            auto range = [&](auto tag) {
                using T = decltype(tag);
                lo = static_cast<double>(std::numeric_limits<T>::lowest());
                hi = static_cast<double>(std::numeric_limits<T>::max());
                return true;
            };
            if (t == rttr::type::get<char>()) return range(char {});
            if (t == rttr::type::get<signed char>()) return range((signed char) {});
            if (t == rttr::type::get<unsigned char>()) return range((unsigned char) {});
            if (t == rttr::type::get<short>()) return range(short {});
            if (t == rttr::type::get<unsigned short>()) return range((unsigned short) {});
            if (t == rttr::type::get<int>()) return range(int {});
            if (t == rttr::type::get<unsigned int>()) return range((unsigned int) {});
            if (t == rttr::type::get<long>()) return range(long {});
            if (t == rttr::type::get<unsigned long>()) return range((unsigned long) {});
            if (t == rttr::type::get<long long>()) return range((long long) {});
            if (t == rttr::type::get<unsigned long long>()) return range((unsigned long long) {});
            if (t == rttr::type::get<float>()) return range(float {});
            if (t == rttr::type::get<double>()) return range(double {});
            return false;
        }
    } // namespace

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
        // Whole steps only (one per kPixelsPerStep): the value moves — and is
        // written — when the drag crosses into the next step, not on every pixel.
        constexpr double kPixelsPerStep = 4.0;
        double v = _scrubStart + std::round(dx / kPixelsPerStep) * step;
        if (!floating)
        {
            v = std::round(v);
        }
        else if (const QVariant dec = _scrubValue.data(DecimalsRole); dec.isValid())
        {
            const double scale = std::pow(10.0, std::clamp(dec.toInt(), 0, 12));
            v = std::round(v * scale) / scale; // the editor's precision
        }
        else
        {
            // Two digits past the step's own: drops float noise (0.30000000004),
            // keeps steps like 0.25 exact.
            const int digits = std::clamp(static_cast<int>(std::ceil(-std::log10(step))) + 2, 0, 12);
            const double scale = std::pow(10.0, digits);
            v = std::round(v * scale) / scale;
        }
        if (const QVariant lo = _scrubValue.data(MinRole); lo.isValid())
        {
            v = std::max(v, lo.toDouble());
        }
        if (const QVariant hi = _scrubValue.data(MaxRole); hi.isValid())
        {
            v = std::min(v, hi.toDouble());
        }
        // And the type's own range — an unsigned value stops at 0 instead of the
        // drag silently doing nothing below it.
        double typeLo = 0, typeHi = 0;
        if (numericRange(t, typeLo, typeHi))
        {
            v = std::clamp(v, typeLo, typeHi);
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

    void PropertyEditor::_updateScopeIcon()
    {
        const QPalette& pal = _scopeBtn->palette();
        const QColor c = _scopeBtn->isChecked() ? pal.color(QPalette::Active, QPalette::HighlightedText)
                                                : pal.color(QPalette::Active, QPalette::ButtonText);
        _scopeBtn->setIcon(layersIcon(c));
    }

    bool PropertyEditor::eventFilter(QObject* obj, QEvent* ev)
    {
        if (obj == _scopeBtn
            && (ev->type() == QEvent::Polish || ev->type() == QEvent::PaletteChange || ev->type() == QEvent::StyleChange))
        {
            _updateScopeIcon();
            return false;
        }
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
        emit filterTextChanged(text.trimmed());
    }

    void PropertyEditor::_onResetAll()
    {
        _model->resetAll();
    }

    void PropertyEditor::_onContextMenu(const QPoint& pos)
    {
        const QModelIndex proxyIdx = _view->indexAt(pos);
        const QModelIndex srcIdx = proxyIdx.isValid() ? _proxy->mapToSource(proxyIdx) : QModelIndex();
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
        // Freezing makes sense only for a row with a value of its own that could
        // change: not a group header, not a read-only value (it would just stop
        // updating, amber, for no edit that could ever come).
        const bool freezable = !srcIdx.data(IsGroupRole).toBool() && srcIdx.siblingAtColumn(1).data(ReadOnlyRole).toString().isEmpty();
        if (!isReadOnly() && (freezable || hasLocalEdit))
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
