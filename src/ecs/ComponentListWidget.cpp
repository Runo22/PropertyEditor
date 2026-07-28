#include "rpe/ecs/ComponentListWidget.h"

#include "rpe/core/TypeBridge.h"

#include <QEvent>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QScreen>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

// rpe_gui is a STATIC library; its embedded .qrc is only auto-registered if some
// referenced symbol pulls in the resource object. Force it once at startup.
static const bool g_rpeResourcesInited = [] {
    Q_INIT_RESOURCE(rpe_resources);
    return true;
}();

namespace rpe
{

    namespace
    {
        // Cached icons (loaded from the embedded Qt resource — :/rpe/icons/*.png).
        const QPixmap& removePixmap()
        {
            static const QPixmap pm(QStringLiteral(":/rpe/icons/remove.png"));
            return pm;
        }
        const QPixmap& confirmPixmap()
        {
            static const QPixmap pm(QStringLiteral(":/rpe/icons/confirm.png"));
            return pm;
        }

        // Forwards key presses to a callback; used to drive the add-popup's tree
        // from the filter box (arrows/enter/escape) without moving focus away from
        // typing. Plain QObject override — no Q_OBJECT/moc needed.
        class PopupKeyRouter : public QObject
        {
        public:
            using QObject::QObject;
            std::function<bool(QKeyEvent*)> onKey; // return true = consumed

            bool eventFilter(QObject* obj, QEvent* ev) override
            {
                if (ev->type() == QEvent::KeyPress && onKey && onKey(static_cast<QKeyEvent*>(ev)))
                {
                    return true;
                }
                return QObject::eventFilter(obj, ev);
            }
        };

        // Leaf (unscoped) name for display, from a full path "game.Transform" /
        // "game::Transform" → "Transform".
        QString componentLeaf(const QString& path)
        {
            int p = path.lastIndexOf(QStringLiteral("::"));
            int s = 2;
            if (p < 0)
            {
                p = path.lastIndexOf(QLatin1Char('.'));
                s = 1;
            }
            return p >= 0 ? path.mid(p + s) : path;
        }

        // Item-data roles shared by the list rows. UserRole keeps its historical
        // meaning (full path in mirror mode / int index in direct mode).
        enum CompItemRole
        {
            CompKindRole = Qt::UserRole + 1,  // int (MirrorChannel::RowKind)
            CompRawIdRole = Qt::UserRole + 2, // qulonglong flecs id
        };

        // Row display text: data/tag rows show the leaf name; pairs "Rel \u2192 Target".
        QString rowDisplay(const MirrorChannel::ComponentRow& r)
        {
            if (r.kind == MirrorChannel::RowKind::Pair || r.kind == MirrorChannel::RowKind::PairData)
            {
                return componentLeaf(r.name) + QStringLiteral(" \u2192 ") + r.pairTarget;
            }
            return componentLeaf(r.name);
        }

        // Sort: data first, then tags, then pairs; each group alphabetically by the
        // displayed leaf, full name (then pair target) as deterministic tiebreaks.
        // Visual order: data, data-carrying pairs, tags, dataless pairs.
        int rowRank(MirrorChannel::RowKind k)
        {
            switch (k)
            {
            case MirrorChannel::RowKind::Data: return 0;
            case MirrorChannel::RowKind::PairData: return 1;
            case MirrorChannel::RowKind::Tag: return 2;
            case MirrorChannel::RowKind::Pair: return 3;
            }
            return 4;
        }

        bool rowLess(const MirrorChannel::ComponentRow& a, const MirrorChannel::ComponentRow& b)
        {
            if (a.kind != b.kind)
            {
                return rowRank(a.kind) < rowRank(b.kind);
            }
            const int l = componentLeaf(a.name).compare(componentLeaf(b.name), Qt::CaseInsensitive);
            if (l != 0)
            {
                return l < 0;
            }
            const int f = a.name.compare(b.name, Qt::CaseInsensitive);
            if (f != 0)
            {
                return f < 0;
            }
            return a.pairTarget.compare(b.pairTarget, Qt::CaseInsensitive) < 0;
        }

        // Per-row remove affordance with a two-step confirm. A row's trailing icon
        // is normally the trash (remove.png). Clicking it once "arms" that row: the
        // icon becomes a warning-coloured button (confirm.png). Clicking again
        // removes. Selecting another row, clicking elsewhere, or focus loss reverts
        // the armed row (handled by the owning widget clearing `confirmName`).
        // Plain delegate (no Q_OBJECT) — the widget supplies callbacks.
        class RemoveButtonDelegate : public QStyledItemDelegate
        {
        public:
            using QStyledItemDelegate::QStyledItemDelegate;

            bool enabled = false;
            QString confirmName;       // component currently armed for delete
            QListWidget* list = nullptr;
            std::function<void(const QString&, int kind, qulonglong rawId)> onRemove;

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
                if (!confirmName.isEmpty())
                {
                    confirmName.clear();
                    repaint();
                }
            }

            void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& index) const override
            {
                const int kind = index.data(CompKindRole).toInt();

                // Reserve room on the right so the text doesn't run under the icon,
                // plus a badge chip for tag/pair rows.
                QStyleOptionViewItem o(opt);
                if (enabled)
                    o.rect.adjust(0, 0, -o.rect.height(), 0);
                if (kind == 1 || kind == 2)
                {
                    // Presence rows (tag / dataless pair): nothing to inspect —
                    // render dimmed + italic so they read as state, not data.
                    o.font.setItalic(true);
                    QColor dim = opt.palette.color(QPalette::Text);
                    dim.setAlpha(150);
                    o.palette.setColor(QPalette::Text, dim);
                }
                if (kind != 0)
                {
                    o.rect.adjust(0, 0, -o.rect.height() - 30, 0);
                }
                QStyledItemDelegate::paint(p, o, index);

                if (kind != 0)
                {
                    // Badge chip ("tag" / "pair"), right-aligned before the trash.
                    const QString label = kind == 1 ? QStringLiteral("tag") : QStringLiteral("pair");
                    const int reserve = enabled ? opt.rect.height() : 0;
                    QRect badge(opt.rect.right() - reserve - 32, opt.rect.top() + 3,
                                30, opt.rect.height() - 6);
                    p->save();
                    p->setRenderHint(QPainter::Antialiasing, true);
                    QColor bg = opt.palette.color(QPalette::Text);
                    bg.setAlpha(28);
                    p->setPen(Qt::NoPen);
                    p->setBrush(bg);
                    p->drawRoundedRect(badge, 4, 4);
                    QColor fg = opt.palette.color(QPalette::Text);
                    fg.setAlpha(170);
                    p->setPen(fg);
                    QFont bf = opt.font;
                    bf.setPointSizeF(bf.pointSizeF() * 0.8);
                    p->setFont(bf);
                    p->drawText(badge, Qt::AlignCenter, label);
                    p->restore();
                }
                if (!enabled)
                    return;

                // Draw the icon at 70% of the row-height square, centred. The
                // clickable area (glyphRect) stays full height for an easy hit target.
                const QRect cell = glyphRect(opt);
                const int isz = cell.height() * 70 / 100;
                const QRect btn(cell.x() + (cell.width() - isz) / 2, cell.y() + (cell.height() - isz) / 2, isz, isz);
                const QString name = index.data(Qt::DisplayRole).toString();
                p->save();
                p->setRenderHint(QPainter::Antialiasing, true);
                p->setRenderHint(QPainter::SmoothPixmapTransform, true);
                if (name == confirmName)
                {
                    // Armed: warning-coloured button + white check ("click to confirm").
                    p->setPen(Qt::NoPen);
                    p->setBrush(QColor(0xD9, 0x53, 0x4F));
                    p->drawRoundedRect(btn, 3, 3);
                    p->drawPixmap(btn, confirmPixmap());
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
                    p->drawPixmap(btn, removePixmap());
                }
                p->restore();
            }

            bool editorEvent(QEvent* ev, QAbstractItemModel*, const QStyleOptionViewItem& opt, const QModelIndex& index) override
            {
                if (!enabled || ev->type() != QEvent::MouseButtonRelease)
                    return false;
                auto* me = static_cast<QMouseEvent*>(ev);
                const QString name = index.data(Qt::DisplayRole).toString();
                if (me->button() == Qt::LeftButton && glyphRect(opt).contains(me->pos()))
                {
                    if (name == confirmName)
                    {
                        confirmName.clear();
                        if (onRemove)
                        {
                            onRemove(name, index.data(CompKindRole).toInt(),
                                     index.data(CompRawIdRole).toULongLong()); // second click → remove
                        }
                    }
                    else
                    {
                        confirmName = name; // first click → arm confirm
                    }
                    repaint();
                    return true; // consume — no selection change / edit
                }
                clearConfirm(); // clicked a row but not its button → revert
                return false;
            }
        };
    } // namespace

    ComponentListWidget::ComponentListWidget(QWidget* parent)
        : QWidget(parent)
    {
        _setupUi();
    }

    void ComponentListWidget::_setupUi()
    {
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(2);

        // Header row: "Components" + a labelled "Add" button.
        auto* headerRow = new QHBoxLayout();
        headerRow->setContentsMargins(0, 0, 0, 0);
        auto* header = new QLabel(tr("Components"), this);
        header->setStyleSheet(QStringLiteral("font-weight: bold; padding: 2px 4px;"));
        headerRow->addWidget(header, 1);

        _addBtn = new QToolButton(this);
        _addBtn->setText(tr("Add"));
        _addBtn->setIcon(QIcon(QStringLiteral(":/rpe/icons/add.png")));
        _addBtn->setIconSize(QSize(14, 14)); // proportional to the label
        _addBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        // A little breathing room so the "+ Add" isn't cramped against the glyph/edges.
        _addBtn->setStyleSheet(QStringLiteral("QToolButton { padding: 2px 6px; }"));
        _addBtn->setToolTip(tr("Add a component to the selected entity"));
        _addBtn->setAutoRaise(true);
        _addBtn->setVisible(false); // shown only when editing is enabled
        headerRow->addWidget(_addBtn, 0);
        layout->addLayout(headerRow);

        _list = new QListWidget(this);
        _list->setMouseTracking(true); // so the row icon can highlight on hover
        auto* del = new RemoveButtonDelegate(_list);
        del->list = _list;
        del->onRemove = [this](const QString& name, int /*kind*/, qulonglong rawId) {
            // Prefer the flecs id for EVERY row that carries one (data, tag and pair
            // rows all do): it removes exactly the clicked component. By-name removal
            // matches on the short leaf name, so a leaf collision (game.Transform vs
            // physics.Transform) could take the wrong one — only used as a fallback
            // for rows with no id.
            if (rawId != 0)
                emit removeComponentIdRequested(rawId);
            else if (!name.isEmpty())
                emit removeComponentRequested(name);
        };
        _list->setItemDelegate(del);
        _rowDelegate = del;
        // Revert a pending delete-confirm when the list loses focus (clicked away).
        _list->installEventFilter(this);
        _list->viewport()->installEventFilter(this);
        layout->addWidget(_list, 1);

        connect(_list, &QListWidget::currentItemChanged, this, &ComponentListWidget::_onSelectionChanged);
        connect(_addBtn, &QToolButton::clicked, this, &ComponentListWidget::_onAddClicked);
    }

    bool ComponentListWidget::eventFilter(QObject* obj, QEvent* ev)
    {
        if ((obj == _list || obj == _list->viewport()) && ev->type() == QEvent::FocusOut)
        {
            static_cast<RemoveButtonDelegate*>(_rowDelegate)->clearConfirm();
        }
        return QWidget::eventFilter(obj, ev);
    }

    void ComponentListWidget::setComponentEditingEnabled(bool on)
    {
        _editingEnabled = on;
        _addBtn->setVisible(on);
        auto* del = static_cast<RemoveButtonDelegate*>(_rowDelegate);
        del->enabled = on;
        del->clearConfirm();
        _list->viewport()->update();
    }

    void ComponentListWidget::setAddableComponents(const QStringList& names)
    {
        QVector<MirrorChannel::CatalogEntry> entries;
        entries.reserve(names.size());
        for (const QString& n : names)
        {
            entries.append(MirrorChannel::CatalogEntry { n, false });
        }
        setAddableEntries(entries);
    }

    void ComponentListWidget::setAddableEntries(const QVector<MirrorChannel::CatalogEntry>& entries)
    {
        _addable = entries;
    }

    void ComponentListWidget::_onAddClicked()
    {
        // A small popup: a filter box over a tree of addable components grouped by
        // namespace. Qt::Popup closes on click-outside; WA_DeleteOnClose frees it.
        auto* popup = new QFrame(this, Qt::Popup);
        popup->setObjectName(QStringLiteral("rpeAddPopup")); // so a stylesheet can target it
        popup->setAttribute(Qt::WA_DeleteOnClose);
        popup->setFrameShape(QFrame::StyledPanel);
        auto* lay = new QVBoxLayout(popup);
        lay->setContentsMargins(4, 4, 4, 4);
        lay->setSpacing(4);

        auto* search = new QLineEdit(popup);
        search->setPlaceholderText(tr("Filter components…"));
        search->setClearButtonEnabled(true);
        lay->addWidget(search);

        auto* tree = new QTreeWidget(popup);
        tree->setHeaderHidden(true);
        tree->setRootIsDecorated(true);
        lay->addWidget(tree);

        if (_addable.isEmpty())
        {
            auto* none = new QTreeWidgetItem(tree, { tr("(no components available)") });
            none->setFlags(Qt::NoItemFlags);
        }
        else
        {
            // Group by namespace (everything before the last "::" or "."); the leaf
            // is shown, and the full catalogued name is carried for the add request.
            QHash<QString, QTreeWidgetItem*> groups;
            for (const MirrorChannel::CatalogEntry& entry : _addable)
            {
                const QString& full = entry.path;
                int cut = full.lastIndexOf(QStringLiteral("::"));
                int sep = 2;
                if (cut < 0)
                {
                    cut = full.lastIndexOf(QLatin1Char('.'));
                    sep = 1;
                }
                // Tags group under one "(tags)" node — they are presence markers,
                // not namespaced data components.
                const QString ns = entry.tag ? tr("(tags)") : (cut >= 0 ? full.left(cut) : tr("(global)"));
                const QString leaf = cut >= 0 ? full.mid(cut + sep) : full;
                QTreeWidgetItem*& g = groups[ns];
                if (!g)
                {
                    g = new QTreeWidgetItem(tree, { ns });
                    g->setFlags(Qt::ItemIsEnabled);
                    g->setExpanded(true);
                }
                auto* item = new QTreeWidgetItem(g, { leaf });
                item->setData(0, Qt::UserRole, full);
                if (entry.tag)
                {
                    QFont f = item->font(0);
                    f.setItalic(true);
                    item->setFont(0, f);
                }
            }
        }

        auto activate = [this, popup](QTreeWidgetItem* item) {
            if (!item)
                return;
            const QString full = item->data(0, Qt::UserRole).toString();
            if (!full.isEmpty())
            {
                emit addComponentRequested(full);
                popup->close();
            }
        };
        connect(tree, &QTreeWidget::itemClicked, this, [activate](QTreeWidgetItem* item, int) { activate(item); });
        connect(tree, &QTreeWidget::itemActivated, this, [activate](QTreeWidgetItem* item, int) { activate(item); });

        // The selectable (visible, non-group) options, in top-to-bottom order —
        // the sequence the arrow keys walk and Enter picks from.
        auto visibleLeaves = [tree]() {
            QList<QTreeWidgetItem*> out;
            for (int i = 0; i < tree->topLevelItemCount(); ++i)
            {
                QTreeWidgetItem* g = tree->topLevelItem(i);
                if (g->isHidden())
                    continue;
                for (int j = 0; j < g->childCount(); ++j)
                {
                    QTreeWidgetItem* c = g->child(j);
                    if (!c->isHidden() && !c->data(0, Qt::UserRole).toString().isEmpty())
                        out.append(c);
                }
            }
            return out;
        };

        connect(search, &QLineEdit::textChanged, tree, [tree, visibleLeaves](const QString& q) {
            const QString s = q.trimmed();
            for (int i = 0; i < tree->topLevelItemCount(); ++i)
            {
                QTreeWidgetItem* g = tree->topLevelItem(i);
                int shown = 0;
                for (int j = 0; j < g->childCount(); ++j)
                {
                    QTreeWidgetItem* c = g->child(j);
                    const bool match = s.isEmpty() || c->text(0).contains(s, Qt::CaseInsensitive);
                    c->setHidden(!match);
                    shown += match ? 1 : 0;
                }
                // Hide a group header only if it has children and none are showing.
                g->setHidden(g->childCount() > 0 && shown == 0);
                if (shown)
                    g->setExpanded(true);
            }
            // Highlight the best (first visible) match so Enter picks it directly.
            const QList<QTreeWidgetItem*> leaves = visibleLeaves();
            tree->setCurrentItem(leaves.isEmpty() ? nullptr : leaves.first());
        });

        // Keyboard driving from the filter box: Up/Down walk the visible options
        // (focus stays in the box so typing continues), Enter adds the highlighted
        // option — or the best match when none is highlighted — and Esc closes.
        auto* keys = new PopupKeyRouter(popup);
        keys->onKey = [tree, popup, visibleLeaves, activate](QKeyEvent* ke) -> bool {
            switch (ke->key())
            {
            case Qt::Key_Down:
            case Qt::Key_Up:
            {
                const QList<QTreeWidgetItem*> leaves = visibleLeaves();
                if (leaves.isEmpty())
                    return true;
                int idx = leaves.indexOf(tree->currentItem());
                idx = (ke->key() == Qt::Key_Down) ? qMin(idx + 1, leaves.size() - 1) : qMax(idx - 1, 0);
                tree->setCurrentItem(leaves[idx]);
                tree->scrollToItem(leaves[idx]);
                return true;
            }
            case Qt::Key_Return:
            case Qt::Key_Enter:
            {
                QTreeWidgetItem* cur = tree->currentItem();
                if (!cur || cur->isHidden() || cur->data(0, Qt::UserRole).toString().isEmpty())
                {
                    const QList<QTreeWidgetItem*> leaves = visibleLeaves();
                    cur = leaves.isEmpty() ? nullptr : leaves.first();
                }
                activate(cur);
                return true;
            }
            case Qt::Key_Escape:
                popup->close();
                return true;
            default:
                return false; // let the line edit handle typing
            }
        };
        search->installEventFilter(keys);

        // Pre-highlight the first option so a bare Enter adds it immediately.
        {
            const QList<QTreeWidgetItem*> leaves = visibleLeaves();
            if (!leaves.isEmpty())
                tree->setCurrentItem(leaves.first());
        }

        popup->setMinimumWidth(qMax(200, _list->width()));

        // Position it under the button, then clamp fully inside the screen so it
        // never overflows the window edge. The button is right-aligned in the
        // header, so prefer aligning the popup's RIGHT edge to the button's (grows
        // leftward), and flip above if there's no room below.
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

    void ComponentListWidget::setEntity(flecs::world* world, flecs::entity e)
    {
        _components.clear();
        _mirrorRows.clear();

        // Collect under the guard (entity/component reads touch the world);
        // populate the widget afterwards without holding it.
        QStringList names;
        QVector<int> dataKinds; // 0 = component, 3 = data-carrying pair (badge)
        QVector<MirrorChannel::ComponentRow> extraRows; // tags + dataless pairs
        withGuard(_guard, [&] {
            if (!world || !e.is_alive())
            {
                return;
            }
            e.each([&](flecs::id id) {
                // Pairs. flecs' ecs_get_typeid says which side carries data (the
                // relation first, else the target): such pairs become SELECTABLE,
                // editable rows; dataless ones stay badges. flecs-internal relations
                // (ChildOf/IsA/Identifier under the flecs scope) stay hidden.
                if (id.is_pair())
                {
                    const flecs::entity rel = id.first();
                    const char* rn = rel.name();
                    if (!rn || rn[0] == '\0')
                    {
                        return;
                    }
                    const flecs::string rp = rel.path(".", "");
                    const QString relPath = rp.c_str() ? QString::fromUtf8(rp.c_str()) : QString();
                    if (relPath.startsWith(QStringLiteral("flecs")))
                    {
                        return;
                    }
                    const flecs::entity tgt = id.second();
                    const char* tn = tgt.name();
                    const QString target = (tn && tn[0] != '\0')
                        ? QString::fromUtf8(tn)
                        : QStringLiteral("#%1").arg(static_cast<qulonglong>(tgt.id()));

                    const ecs_entity_t tid = ecs_get_typeid(world->c_ptr(), id.raw_id());
                    if (tid != 0)
                    {
                        const flecs::entity typeEnt = world->entity(tid);
                        const flecs::string tp = typeEnt.path(".", "");
                        const rttr::type rt = TypeBridge::resolveByName(tp.c_str() ? tp.c_str() : "");
                        if (rt.is_valid())
                        {
                            _components.append(ComponentInfo { id, rt });
                            names.append(componentLeaf(relPath) + QStringLiteral(" \u2192 ") + target);
                            dataKinds.append(3);
                            return;
                        }
                    }
                    extraRows.append({ relPath, target, MirrorChannel::RowKind::Pair,
                                       static_cast<qulonglong>(id.raw_id()) });
                    return;
                }
                if (!id.is_entity())
                {
                    return;
                }
                flecs::entity comp = id.entity();
                const char* raw = comp.name();
                if (!raw || raw[0] == '\0')
                {
                    return;
                }
                const flecs::string fp = comp.path(".", "");
                const QString fullPath = fp.c_str() ? QString::fromUtf8(fp.c_str()) : QString();
                if (fullPath.startsWith(QStringLiteral("flecs")))
                {
                    return;
                }

                // Zero-size tags (and plain attached entities) become TAG rows: pure
                // presence, nothing to inspect — and get_mut on a dataless id asserts
                // in debug flecs, so they must never enter the data path.
                const flecs::Component* cd = comp.try_get<flecs::Component>();
                if (!cd || cd->size <= 0)
                {
                    extraRows.append({ fullPath, QString(), MirrorChannel::RowKind::Tag,
                                       static_cast<qulonglong>(id.raw_id()) });
                    return;
                }

                // Only components we can actually inspect/edit (a TypeBridge wrapper
                // turns the raw component pointer into an RTTR instance). resolveByName
                // bridges flecs' short component name to the registered RTTR type, which
                // may be scoped or registered under a different name.
                const rttr::type t = TypeBridge::resolveByName(raw);
                if (!t.is_valid())
                {
                    return;
                }

                _components.append(ComponentInfo { id, t });
                names.append(QString::fromUtf8(raw));
                dataKinds.append(0);
            });
        });

        // Sort names and their parallel ComponentInfos together, alphabetically by
        // the displayed leaf name (item UserRole stays the index into _components).
        {
            QVector<int> order(names.size());
            for (int i = 0; i < names.size(); ++i)
            {
                order[i] = i;
            }
            std::sort(order.begin(), order.end(), [&](int a, int b) {
                const int c = componentLeaf(names[a]).compare(componentLeaf(names[b]), Qt::CaseInsensitive);
                // Full name breaks leaf ties (e.g. physics::Collider vs render::Collider)
                // so the order is deterministic — std::sort isn't stable.
                return c != 0 ? c < 0 : names[a].compare(names[b], Qt::CaseInsensitive) < 0;
            });
            QStringList sortedNames;
            QVector<ComponentInfo> sortedInfos;
            QVector<int> sortedKinds;
            sortedNames.reserve(names.size());
            sortedInfos.reserve(_components.size());
            sortedKinds.reserve(dataKinds.size());
            for (int i : order)
            {
                sortedNames.append(names[i]);
                sortedInfos.append(_components[i]);
                sortedKinds.append(dataKinds[i]);
            }
            names = sortedNames;
            _components = sortedInfos;
            dataKinds = sortedKinds;
        }

        std::sort(extraRows.begin(), extraRows.end(), rowLess);

        _list->blockSignals(true);
        _list->clear();
        for (int i = 0; i < names.size(); ++i)
        {
            auto* item = new QListWidgetItem(names[i], _list);
            item->setData(Qt::UserRole, i);
            item->setData(CompKindRole, dataKinds[i]);
            item->setData(CompRawIdRole, static_cast<qulonglong>(_components[i].id.raw_id()));
        }
        for (const auto& r : extraRows)
        {
            auto* item = new QListWidgetItem(rowDisplay(r), _list);
            item->setData(CompKindRole, static_cast<int>(r.kind));
            item->setData(CompRawIdRole, r.rawId);
            item->setFlags(Qt::ItemIsEnabled); // presence row: removable, not selectable
        }
        _list->blockSignals(false);
        if (!names.isEmpty())
        {
            _list->setCurrentRow(0); // auto-select first DATA row → drives the editor
        }
        else
        {
            emit componentDeselected();
        }
    }

    void ComponentListWidget::setWorldAccess(AccessGuard guard)
    {
        _guard = std::move(guard);
    }

    void ComponentListWidget::setComponentNames(const QStringList& namesIn)
    {
        QVector<MirrorChannel::ComponentRow> rows;
        rows.reserve(namesIn.size());
        for (const QString& n : namesIn)
        {
            rows.append(MirrorChannel::ComponentRow { n, QString(), MirrorChannel::RowKind::Data, 0 });
        }
        setComponentRows(rows);
    }

    void ComponentListWidget::setComponentRows(const QVector<MirrorChannel::ComponentRow>& rowsIn)
    {
        // Data components first (alphabetical), then tags, then pairs.
        QVector<MirrorChannel::ComponentRow> rows = rowsIn;
        std::sort(rows.begin(), rows.end(), rowLess);

        if (rows == _mirrorRows)
        {
            return; // unchanged → keep selection, no flicker
        }
        // Drop a pending delete-confirm (keyed on the display text) if its row is
        // no longer shown.
        if (auto* del = static_cast<RemoveButtonDelegate*>(_rowDelegate); del && !del->confirmName.isEmpty())
        {
            bool stillThere = false;
            for (const auto& r : rows)
                stillThere |= (rowDisplay(r) == del->confirmName);
            if (!stillThere)
                del->clearConfirm();
        }
        _mirrorRows = rows;
        _components.clear(); // mirror mode has no world-backed infos

        // Preserve selection across the rebuild by PATH (Qt::UserRole).
        const QString prevSelPath = _list->currentItem() ? _list->currentItem()->data(Qt::UserRole).toString() : QString();

        _list->blockSignals(true);
        _list->clear();
        QListWidgetItem* reselect = nullptr;
        QListWidgetItem* firstData = nullptr;
        for (const auto& r : rows)
        {
            auto* item = new QListWidgetItem(rowDisplay(r), _list);
            item->setData(Qt::UserRole, r.key()); // unique identity (string) → mirror mode
            item->setData(CompKindRole, static_cast<int>(r.kind));
            item->setData(CompRawIdRole, r.rawId);
            const bool selectable = r.kind == MirrorChannel::RowKind::Data
                || r.kind == MirrorChannel::RowKind::PairData;
            if (selectable)
            {
                if (!firstData)
                {
                    firstData = item;
                }
                if (r.key() == prevSelPath)
                {
                    reselect = item;
                }
                if (r.kind == MirrorChannel::RowKind::PairData)
                {
                    item->setToolTip(QStringLiteral("(%1, %2) — pair carrying %3")
                                         .arg(r.name, r.pairTarget, r.typeName));
                }
            }
            else
            {
                // Presence rows: nothing to inspect — enabled (so the trash works)
                // but never selectable/current.
                item->setFlags(Qt::ItemIsEnabled);
                item->setToolTip(r.kind == MirrorChannel::RowKind::Pair
                                     ? QStringLiteral("(%1, %2) — relationship pair (no data)").arg(r.name, r.pairTarget)
                                     : QStringLiteral("%1 — tag (no data)").arg(r.name));
            }
        }
        _list->blockSignals(false);

        if (reselect)
        {
            _list->setCurrentItem(reselect);
        }
        else if (firstData)
        {
            _list->setCurrentItem(firstData);
        }
        else
        {
            emit componentDeselected();
        }
    }

    QString ComponentListWidget::currentComponentName() const
    {
        auto* item = _list->currentItem();
        if (!item)
        {
            return {};
        }
        // Mirror mode stores the full path (string) in UserRole; direct mode stores
        // an int index. Return the path in mirror mode, else the displayed leaf.
        const QVariant ud = item->data(Qt::UserRole);
        return ud.type() == QVariant::String ? ud.toString() : item->text();
    }

    void ComponentListWidget::clearEntity()
    {
        _components.clear();
        _mirrorRows.clear();
        _list->clear();
        emit componentDeselected();
    }

    void ComponentListWidget::_onSelectionChanged()
    {
        // Selecting a different component reverts any pending delete-confirm.
        static_cast<RemoveButtonDelegate*>(_rowDelegate)->clearConfirm();
        auto* item = _list->currentItem();
        if (!item)
        {
            emit componentDeselected();
            return;
        }
        // Mirror mode: UserRole holds the full path (string) → emit that so the
        // browser resolves the exact type. Direct mode: UserRole holds an int index
        // into _components → emit the world-backed info.
        const QVariant ud = item->data(Qt::UserRole);
        if (ud.type() == QVariant::String)
        {
            emit componentNameSelected(ud.toString());
        }
        else
        {
            emit componentNameSelected(item->text());
            const int idx = ud.toInt();
            if (idx >= 0 && idx < _components.size())
            {
                emit componentSelected(_components[idx]);
            }
        }
    }

} // namespace rpe
