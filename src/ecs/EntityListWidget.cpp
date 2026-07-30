#include "rpe/ecs/EntityListWidget.h"

#include "rpe/core/TypeBridge.h"

#include <QAction>
#include <QApplication>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

namespace rpe
{

    namespace
    {
        // Short (unscoped) form of a name: the segment after the last "::".
        QString shortName(const QString& s)
        {
            const int pos = s.lastIndexOf(QStringLiteral("::"));
            return pos >= 0 ? s.mid(pos + 2) : s;
        }

        // Trim a trailing whitespace-delimited "prefab" (any case) — matches
        // EcsMirror so direct and mirror instance labels read identically.
        QString trimPrefabSuffix(QString s)
        {
            s = s.trimmed();
            static const QString suffix = QStringLiteral(" prefab");
            if (s.size() > suffix.size()
                && s.right(suffix.size()).compare(suffix, Qt::CaseInsensitive) == 0)
            {
                s.chop(suffix.size());
                s = s.trimmed();
            }
            return s;
        }

        // Display label for an entity: its name, else its prefab's name + id, else
        // just the id. Matches EcsMirror so direct and mirror modes look identical.
        QString entityLabel(const flecs::entity& e)
        {
            const char* n = e.name();
            if (n && n[0] != '\0')
            {
                return QString::fromUtf8(n);
            }
            const flecs::entity prefab = e.target(flecs::IsA);
            if (prefab.is_valid())
            {
                const char* pn = prefab.name();
                if (pn && pn[0] != '\0')
                {
                    return QStringLiteral("%1  #%2").arg(trimPrefabSuffix(QString::fromUtf8(pn))).arg(e.id());
                }
            }
            return QStringLiteral("#%1").arg(e.id());
        }

        // Per-row trash affordance for the entity list, with the same two-step
        // confirm as the component list: first click on the trailing glyph arms the
        // row (warning-coloured check), a second click deletes; clicking elsewhere or
        // losing focus reverts. Keyed on the row's entity id (Qt::UserRole).
        class EntityTrashDelegate : public QStyledItemDelegate
        {
        public:
            using QStyledItemDelegate::QStyledItemDelegate;

            bool enabled = false;
            qulonglong confirmId = 0; // entity currently armed for delete (0 = none)
            QListWidget* list = nullptr;
            std::function<void(qulonglong)> onRemove;

            static QRect glyphRect(const QStyleOptionViewItem& opt)
            {
                const int s = opt.rect.height();
                return QRect(opt.rect.right() - s, opt.rect.top(), s, s);
            }
            void repaint() const
            {
                if (list)
                    list->viewport()->update();
            }
            void clearConfirm()
            {
                if (confirmId != 0)
                {
                    confirmId = 0;
                    repaint();
                }
            }

            void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& index) const override
            {
                QStyleOptionViewItem o(opt);
                if (enabled)
                    o.rect.adjust(0, 0, -o.rect.height(), 0); // reserve the glyph square
                QStyledItemDelegate::paint(p, o, index);
                if (!enabled)
                    return;

                const QRect cell = glyphRect(opt);
                const int isz = cell.height() * 70 / 100;
                const QRect btn(cell.x() + (cell.width() - isz) / 2, cell.y() + (cell.height() - isz) / 2, isz, isz);
                const qulonglong id = index.data(Qt::UserRole).toULongLong();
                p->save();
                p->setRenderHint(QPainter::Antialiasing, true);
                p->setRenderHint(QPainter::SmoothPixmapTransform, true);
                static const QPixmap trashPm(QStringLiteral(":/rpe/icons/remove.png"));
                static const QPixmap confirmPm(QStringLiteral(":/rpe/icons/confirm.png"));
                if (id != 0 && id == confirmId)
                {
                    p->setPen(Qt::NoPen);
                    p->setBrush(QColor(0xD9, 0x53, 0x4F));
                    p->drawRoundedRect(btn, 3, 3);
                    p->drawPixmap(btn, confirmPm);
                }
                else
                {
                    const bool hover = opt.state & QStyle::State_MouseOver;
                    if (hover)
                    {
                        QColor bg = opt.palette.color(QPalette::Text);
                        bg.setAlpha(32);
                        p->setPen(Qt::NoPen);
                        p->setBrush(bg);
                        p->drawRoundedRect(btn, 3, 3);
                    }
                    p->setOpacity(hover ? 1.0 : 0.7);
                    p->drawPixmap(btn, trashPm);
                }
                p->restore();
            }

            bool editorEvent(QEvent* ev, QAbstractItemModel*, const QStyleOptionViewItem& opt, const QModelIndex& index) override
            {
                if (!enabled || ev->type() != QEvent::MouseButtonRelease)
                    return false;
                auto* me = static_cast<QMouseEvent*>(ev);
                const qulonglong id = index.data(Qt::UserRole).toULongLong();
                if (me->button() == Qt::LeftButton && id != 0 && glyphRect(opt).contains(me->pos()))
                {
                    if (id == confirmId)
                    {
                        confirmId = 0;
                        if (onRemove)
                            onRemove(id); // second click → delete
                    }
                    else
                    {
                        confirmId = id; // first click → arm
                    }
                    repaint();
                    return true; // consume: no selection change
                }
                clearConfirm();
                return false;
            }
        };
    } // namespace

    EntityListWidget::EntityListWidget(QWidget* parent)
        : QWidget(parent)
    {
        _setupUi();
        _timer = new QTimer(this);
        _timer->setInterval(500);
        connect(_timer, &QTimer::timeout, this, &EntityListWidget::_refresh);
    }

    void EntityListWidget::_setupUi()
    {
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(2);

        // Header row: "Entities" + a labelled "Add" spawn button (shown on demand).
        auto* headerRow = new QHBoxLayout();
        headerRow->setContentsMargins(0, 0, 0, 0);
        auto* header = new QLabel(tr("Entities"), this);
        header->setStyleSheet(QStringLiteral("font-weight: bold; padding: 2px 4px;"));
        headerRow->addWidget(header, 1);

        _addBtn = new QToolButton(this);
        _addBtn->setText(tr("Add"));
        _addBtn->setIcon(QIcon(QStringLiteral(":/rpe/icons/add.png")));
        _addBtn->setIconSize(QSize(14, 14));
        _addBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        _addBtn->setStyleSheet(QStringLiteral("QToolButton { padding: 2px 6px; }"));
        _addBtn->setToolTip(tr("Spawn a new entity from a prefab"));
        _addBtn->setAutoRaise(true);
        _addBtn->setVisible(false); // shown only when entity-adding is enabled
        headerRow->addWidget(_addBtn, 0);
        layout->addLayout(headerRow);

        _filterEdit = new QLineEdit(this);
        _filterEdit->setPlaceholderText(tr("Filter entities…"));
        _filterEdit->setClearButtonEnabled(true);
        layout->addWidget(_filterEdit);

        _list = new QListWidget(this);
        _list->setMouseTracking(true); // so the trash glyph highlights on hover
        auto* del = new EntityTrashDelegate(_list);
        del->list = _list;
        del->onRemove = [this](qulonglong id) {
            if (id != 0)
                emit removeEntityRequested(id);
        };
        _list->setItemDelegate(del);
        _rowDelegate = del;
        _list->setContextMenuPolicy(Qt::CustomContextMenu);
        layout->addWidget(_list, 1);

        connect(_list, &QListWidget::currentItemChanged, this, &EntityListWidget::_onSelectionChanged);
        connect(_list, &QListWidget::customContextMenuRequested, this, &EntityListWidget::_onContextMenu);
        connect(_filterEdit, &QLineEdit::textChanged, this, &EntityListWidget::_refresh);
        connect(_addBtn, &QToolButton::clicked, this, &EntityListWidget::_onAddEntityClicked);
    }

    void EntityListWidget::setEntityRemovingEnabled(bool on)
    {
        if (auto* del = static_cast<EntityTrashDelegate*>(_rowDelegate))
        {
            del->enabled = on;
            del->clearConfirm();
            _list->viewport()->update();
        }
    }

    void EntityListWidget::setContextActions(const QVector<EntityAction>& actions)
    {
        _contextActions = actions;
    }

    void EntityListWidget::setContextMenuHook(std::function<void(qulonglong, QMenu&)> hook)
    {
        _menuHook = std::move(hook);
    }

    void EntityListWidget::_onContextMenu(const QPoint& pos)
    {
        QListWidgetItem* item = _list->itemAt(pos);
        if (!item)
        {
            return;
        }
        const qulonglong id = item->data(Qt::UserRole).toULongLong();
        if (id == 0)
        {
            return;
        }
        static_cast<EntityTrashDelegate*>(_rowDelegate)->clearConfirm();

        QMenu menu(this);
        const bool removable = static_cast<EntityTrashDelegate*>(_rowDelegate)->enabled;
        if (removable)
        {
            QAction* del = menu.addAction(QIcon(QStringLiteral(":/rpe/icons/remove.png")), tr("Delete entity"));
            connect(del, &QAction::triggered, this, [this, id] { emit removeEntityRequested(id); });
        }
        if (!_contextActions.isEmpty())
        {
            if (removable)
                menu.addSeparator();
            for (const EntityAction& a : _contextActions)
            {
                if (a.label.isEmpty() || !a.callback)
                    continue;
                QAction* act = a.icon.isNull() ? menu.addAction(a.label) : menu.addAction(a.icon, a.label);
                const auto cb = a.callback;
                connect(act, &QAction::triggered, this, [cb, id] { cb(id); });
            }
        }
        // Dynamic host hook: build entity-specific entries at open time.
        if (_menuHook)
        {
            _menuHook(id, menu);
        }
        if (!menu.isEmpty())
        {
            menu.exec(_list->viewport()->mapToGlobal(pos));
        }
    }

    void EntityListWidget::setEntityAddingEnabled(bool on)
    {
        _addBtn->setVisible(on);
    }

    void EntityListWidget::setAddablePrefabs(const QVector<MirrorChannel::PrefabEntry>& prefabs)
    {
        _prefabs = prefabs;
    }

    void EntityListWidget::setPrefabGroupIcons(const QHash<QString, QIcon>& icons)
    {
        _groupIcons = icons;
    }

    void EntityListWidget::_onAddEntityClicked()
    {
        // A small popup: a filter box over a tree of spawnable prefabs, grouped by
        // their group tag (with the host's optional icon on each group header).
        // Qt::Popup closes on click-outside; WA_DeleteOnClose frees it.
        auto* popup = new QFrame(this, Qt::Popup);
        popup->setObjectName(QStringLiteral("rpeAddPopup"));
        popup->setAttribute(Qt::WA_DeleteOnClose);
        popup->setFrameShape(QFrame::StyledPanel);
        auto* lay = new QVBoxLayout(popup);
        lay->setContentsMargins(4, 4, 4, 4);
        lay->setSpacing(4);

        auto* search = new QLineEdit(popup);
        search->setPlaceholderText(tr("Filter prefabs…"));
        search->setClearButtonEnabled(true);
        lay->addWidget(search);

        auto* tree = new QTreeWidget(popup);
        tree->setHeaderHidden(true);
        tree->setRootIsDecorated(true);
        lay->addWidget(tree);

        if (_prefabs.isEmpty())
        {
            auto* none = new QTreeWidgetItem(tree, { tr("(no prefabs available)") });
            none->setFlags(Qt::NoItemFlags);
        }
        else if (std::none_of(_prefabs.cbegin(), _prefabs.cend(),
                              [](const MirrorChannel::PrefabEntry& p) { return !p.group.isEmpty(); }))
        {
            // No grouping configured → a FLAT alphabetical list (the producer already
            // sorts by name), no "(ungrouped)" header cluttering things.
            tree->setRootIsDecorated(false);
            for (const MirrorChannel::PrefabEntry& p : _prefabs)
            {
                auto* item = new QTreeWidgetItem(tree, { p.name });
                item->setData(0, Qt::UserRole, p.id); // the spawn handle
            }
        }
        else
        {
            QHash<QString, QTreeWidgetItem*> groups;
            for (const MirrorChannel::PrefabEntry& p : _prefabs)
            {
                const QString g = p.group.isEmpty() ? tr("(ungrouped)") : p.group;
                QTreeWidgetItem*& node = groups[g];
                if (!node)
                {
                    node = new QTreeWidgetItem(tree, { g });
                    node->setFlags(Qt::ItemIsEnabled);
                    node->setExpanded(true);
                    if (const auto it = _groupIcons.constFind(p.group); it != _groupIcons.constEnd())
                    {
                        node->setIcon(0, it.value());
                    }
                }
                auto* item = new QTreeWidgetItem(node, { p.name });
                item->setData(0, Qt::UserRole, p.id); // the spawn handle
            }
        }

        auto activate = [this, popup](QTreeWidgetItem* item) {
            if (!item)
                return;
            const qulonglong id = item->data(0, Qt::UserRole).toULongLong();
            if (id != 0)
            {
                emit spawnPrefabRequested(id);
                popup->close();
            }
        };
        connect(tree, &QTreeWidget::itemClicked, this, [activate](QTreeWidgetItem* item, int) { activate(item); });
        connect(tree, &QTreeWidget::itemActivated, this, [activate](QTreeWidgetItem* item, int) { activate(item); });

        connect(search, &QLineEdit::textChanged, tree, [tree](const QString& q) {
            const QString s = q.trimmed();
            for (int i = 0; i < tree->topLevelItemCount(); ++i)
            {
                QTreeWidgetItem* top = tree->topLevelItem(i);
                if (top->childCount() == 0)
                {
                    // Flat prefab leaf (grouping off) — filter it directly. A leaf
                    // carries a spawn id; the "(no prefabs)" placeholder does not.
                    if (top->data(0, Qt::UserRole).toULongLong() != 0)
                        top->setHidden(!(s.isEmpty() || top->text(0).contains(s, Qt::CaseInsensitive)));
                    continue;
                }
                int shown = 0;
                for (int j = 0; j < top->childCount(); ++j)
                {
                    QTreeWidgetItem* c = top->child(j);
                    const bool match = s.isEmpty() || c->text(0).contains(s, Qt::CaseInsensitive);
                    c->setHidden(!match);
                    shown += match ? 1 : 0;
                }
                top->setHidden(shown == 0); // hide an empty group header
            }
        });

        // Position it under the button, then clamp fully inside the screen so it
        // never overflows the window edge (the button is right-aligned in the
        // header). Same behaviour as the add-component picker: grow leftward from
        // the button's right edge, flip above if there's no room below.
        popup->resize(240, 300);
        const QPoint anchorBR = _addBtn->mapToGlobal(_addBtn->rect().bottomRight());
        const QScreen* scr = QGuiApplication::screenAt(anchorBR);
        const QRect avail = (scr ? scr : QGuiApplication::primaryScreen())->availableGeometry();
        popup->setMaximumHeight(qMax(140, avail.height() - 40));
        popup->adjustSize();
        const QSize sz = popup->size();

        int x = anchorBR.x() - sz.width();
        int y = anchorBR.y();
        if (y + sz.height() > avail.bottom())
        {
            const int aboveY = _addBtn->mapToGlobal(_addBtn->rect().topRight()).y() - sz.height();
            if (aboveY >= avail.top())
                y = aboveY;
        }
        x = qBound(avail.left(), x, avail.right() - sz.width() + 1);
        y = qBound(avail.top(), y, avail.bottom() - sz.height() + 1);
        popup->move(x, y);
        popup->show();
        search->setFocus();
    }

    void EntityListWidget::setWorld(flecs::world* world)
    {
        _world = world;
        if (_world)
        {
            _timer->start();
        }
        else
        {
            _timer->stop();
        }
        _refresh();
    }

    void EntityListWidget::setRefreshIntervalMs(int ms)
    {
        _timer->setInterval(ms);
    }

    void EntityListWidget::setWorldAccess(AccessGuard guard)
    {
        _guard = std::move(guard);
    }

    void EntityListWidget::stopAutoRefresh()
    {
        _timer->stop();
    }

    void EntityListWidget::setRequiredComponent(const QString& componentName, bool enabled)
    {
        // The filter is configured purely through state now (no UI checkbox) — the
        // host drives it via the browser's Settings. Direct mode applies it in
        // _refresh(); mirror mode applies it on the producer.
        _requiredComponent = componentName;
        _requiredEnabled = enabled;
        _refresh();
    }

    void EntityListWidget::_refresh()
    {
        if (!_world)
        {
            // Mirror mode: entries arrive via setEntries(). A filter-text or
            // required-toggle change must NOT wipe the list — just re-apply the text
            // filter over the entries we already have (clearing the box restores
            // them all). The required-component filter is applied upstream by the
            // mirror, so there is nothing to re-query here.
            _applyTextFilter();
            return;
        }

        const bool filterByComp = !_requiredComponent.isEmpty() && _requiredEnabled;

        // Show every entity carrying at least one bridged component (the ones the
        // inspector can actually display), optionally constrained to those having
        // the required component. Labelled by name / prefab name / id — to match
        // EcsMirror exactly. All world reads happen under the guard; the widget
        // rebuild below does not need it.
        const QString reqShort = filterByComp ? shortName(_requiredComponent) : QString();
        QVector<QPair<qulonglong, QString>> entries;
        withGuard(_guard, [&] {
            flecs::query<> q = _world->query_builder().with(flecs::Any).build();

            q.each([&](flecs::entity e) {
                if (!e.is_alive())
                {
                    return;
                }
                bool hasBridged = false;
                bool hasReq = reqShort.isEmpty();
                e.each([&](flecs::id id) {
                    if (!id.is_entity())
                    {
                        return;
                    }
                    const char* cn = id.entity().name();
                    if (!cn || cn[0] == '\0')
                    {
                        return;
                    }
                    if (TypeBridge::resolveByName(cn).is_valid())
                    {
                        hasBridged = true;
                    }
                    if (!reqShort.isEmpty() && shortName(QString::fromUtf8(cn)) == reqShort)
                    {
                        hasReq = true;
                    }
                });
                if (!hasBridged || !hasReq)
                {
                    return;
                }
                entries.append({ static_cast<qulonglong>(e.id()), entityLabel(e) });
            });
        });

        // Keep the full set; the text filter is applied on top so clearing it
        // restores every entity without re-querying.
        _sourceEntries = entries;
        _applyTextFilter();
    }

    void EntityListWidget::setEntries(const QVector<QPair<qulonglong, QString>>& entries)
    {
        // External feed (mirror mode): remember the full set, then apply the text
        // filter. Editing/clearing the filter re-derives from _sourceEntries.
        _sourceEntries = entries;
        _applyTextFilter();
    }

    bool EntityListWidget::selectById(qulonglong id)
    {
        _requestedId = id;
        for (int i = 0; i < _list->count(); ++i)
        {
            if (_list->item(i)->data(Qt::UserRole).toULongLong() == id)
            {
                _requestedId = 0;
                _list->setCurrentRow(i); // emits _onSelectionChanged → selection signals
                return true;
            }
        }
        return false; // not present yet — applied by _applyEntries when it appears
    }

    QString EntityListWidget::currentLabel() const
    {
        const auto* cur = _list->currentItem();
        return cur ? cur->text() : QString();
    }

    void EntityListWidget::_applyTextFilter()
    {
        const QString filter = _filterEdit->text().trimmed().toLower();
        if (filter.isEmpty())
        {
            _applyEntries(_sourceEntries);
            return;
        }
        QVector<QPair<qulonglong, QString>> filtered;
        for (const auto& e : _sourceEntries)
        {
            if (e.second.toLower().contains(filter))
            {
                filtered.append(e);
            }
        }
        _applyEntries(filtered);
    }

    void EntityListWidget::_applyEntries(const QVector<QPair<qulonglong, QString>>& entriesIn)
    {
        // Show entities sorted alphabetically by label (case-insensitive), id as a
        // stable tiebreaker for same-named entities.
        QVector<QPair<qulonglong, QString>> entries = entriesIn;
        const auto less = [](const QPair<qulonglong, QString>& a, const QPair<qulonglong, QString>& b) {
            const int c = a.second.compare(b.second, Qt::CaseInsensitive);
            return c != 0 ? c < 0 : a.first < b.first;
        };
        std::sort(entries.begin(), entries.end(), less);

        // Skip entirely when the visible set is unchanged — avoids flicker.
        if (entries == _lastEntries)
        {
            return;
        }

        // Remember current selection so it survives the update.
        qulonglong selectedId = 0;
        if (auto* cur = _list->currentItem())
        {
            selectedId = cur->data(Qt::UserRole).toULongLong();
        }

        // DIFF update instead of clear+rebuild: both lists are sorted by the same
        // comparator, so a single merge pass finds exactly the added/removed rows.
        // A dynamic world then costs a handful of row operations per publish, not
        // thousands — a full rebuild of a big list (item churn on the GUI thread,
        // serialized process-wide by the Windows debug heap) was a periodic stall.
        _list->blockSignals(true);
        _list->setUpdatesEnabled(false);
        int row = 0; // widget row cursor (= kept + inserted so far)
        int i = 0;   // index into _lastEntries (mirrors the widget's current rows)
        int j = 0;   // index into the new entries
        while (i < _lastEntries.size() || j < entries.size())
        {
            const bool haveOld = i < _lastEntries.size();
            const bool haveNew = j < entries.size();
            if (haveOld && haveNew && _lastEntries[i] == entries[j])
            {
                ++i;
                ++j;
                ++row; // unchanged row — item (and any selection on it) untouched
            }
            else if (haveOld && (!haveNew || less(_lastEntries[i], entries[j])))
            {
                delete _list->takeItem(row); // removed entity
                ++i;
            }
            else
            {
                auto* item = new QListWidgetItem(entries[j].second);
                item->setData(Qt::UserRole, entries[j].first);
                _list->insertItem(row, item); // added entity
                ++j;
                ++row;
            }
        }
        _lastEntries = entries;
        _list->setUpdatesEnabled(true);
        _list->blockSignals(false);

        // Selection: a pending selectById() request wins; else the user's selection
        // (if its row survived); else auto-select the top row so the panel is never
        // blank. Signals are live here, so listeners hear about real changes only
        // (setCurrentItem on the already-current item does not re-emit).
        QListWidgetItem* reselect = nullptr;
        QListWidgetItem* requested = nullptr;
        for (int r = 0; r < _list->count(); ++r)
        {
            auto* it = _list->item(r);
            const qulonglong id = it->data(Qt::UserRole).toULongLong();
            if (id == selectedId)
            {
                reselect = it;
            }
            if (_requestedId != 0 && id == _requestedId)
            {
                requested = it;
            }
        }
        if (requested)
        {
            _requestedId = 0;
            _list->setCurrentItem(requested);
        }
        else if (reselect)
        {
            _list->setCurrentItem(reselect);
        }
        else if (_list->count() > 0)
        {
            _list->setCurrentRow(0);
        }
        else if (selectedId != 0)
        {
            emit entityDeselected();
        }
    }

    void EntityListWidget::_onSelectionChanged()
    {
        auto* item = _list->currentItem();
        if (!item)
        {
            emit entityDeselected();
            return;
        }
        const auto id = item->data(Qt::UserRole).toULongLong();
        emit entityIdSelected(id); // world-free; used by mirror mode
        if (_world)
        {
            flecs::entity e;
            bool alive = false;
            withGuard(_guard, [&] {
                e = _world->entity(static_cast<flecs::entity_t>(id));
                alive = e.is_alive();
            });
            if (alive)
            {
                emit entitySelected(e);
            }
        }
    }

} // namespace rpe
