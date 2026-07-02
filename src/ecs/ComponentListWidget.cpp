#include "rpe/ecs/ComponentListWidget.h"

#include "rpe/core/TypeBridge.h"

#include <QEvent>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
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
            std::function<void(const QString&)> onRemove;

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
                // Reserve room on the right so the text doesn't run under the icon.
                QStyleOptionViewItem o(opt);
                if (enabled)
                    o.rect.adjust(0, 0, -o.rect.height(), 0);
                QStyledItemDelegate::paint(p, o, index);
                if (!enabled)
                    return;

                // Tight: the button fills the row-height square (1px breathing room),
                // and the icon fills the button, so the trash/check read clearly.
                const QRect btn = glyphRect(opt).adjusted(1, 1, -1, -1);
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
                            onRemove(name); // second click → remove
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
        _addBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        _addBtn->setToolTip(tr("Add a component to the selected entity"));
        _addBtn->setAutoRaise(true);
        _addBtn->setVisible(false); // shown only when editing is enabled
        headerRow->addWidget(_addBtn, 0);
        layout->addLayout(headerRow);

        _list = new QListWidget(this);
        _list->setMouseTracking(true); // so the row icon can highlight on hover
        auto* del = new RemoveButtonDelegate(_list);
        del->list = _list;
        del->onRemove = [this](const QString& name) {
            if (!name.isEmpty())
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
        _addable = names;
    }

    void ComponentListWidget::_onAddClicked()
    {
        // A small popup: a filter box over a tree of addable components grouped by
        // namespace. Qt::Popup closes on click-outside; WA_DeleteOnClose frees it.
        auto* popup = new QFrame(this, Qt::Popup);
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
            for (const QString& full : _addable)
            {
                int cut = full.lastIndexOf(QStringLiteral("::"));
                int sep = 2;
                if (cut < 0)
                {
                    cut = full.lastIndexOf(QLatin1Char('.'));
                    sep = 1;
                }
                const QString ns = cut >= 0 ? full.left(cut) : tr("(global)");
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

        connect(search, &QLineEdit::textChanged, tree, [tree](const QString& q) {
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
        });

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
        _mirrorNames.clear();

        // Collect under the guard (entity/component reads touch the world);
        // populate the widget afterwards without holding it.
        QStringList names;
        withGuard(_guard, [&] {
            if (!world || !e.is_alive())
            {
                return;
            }
            e.each([&](flecs::id id) {
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
            });
        });

        _list->blockSignals(true);
        _list->clear();
        for (int i = 0; i < names.size(); ++i)
        {
            auto* item = new QListWidgetItem(names[i], _list);
            item->setData(Qt::UserRole, i);
        }
        _list->blockSignals(false);
        if (_list->count() > 0)
        {
            _list->setCurrentRow(0); // auto-select first → drives the editor
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

    void ComponentListWidget::setComponentNames(const QStringList& names)
    {
        if (names == _mirrorNames)
        {
            return; // unchanged → keep selection, no flicker
        }
        // `names` are full scoped paths; the rows DISPLAY the leaf but carry the
        // path (Qt::UserRole) so selection/resolution stay unambiguous.
        // Drop a pending delete-confirm (keyed on the leaf) if it is no longer shown.
        if (auto* del = static_cast<RemoveButtonDelegate*>(_rowDelegate); del && !del->confirmName.isEmpty())
        {
            bool stillThere = false;
            for (const QString& path : names)
                stillThere |= (componentLeaf(path) == del->confirmName);
            if (!stillThere)
                del->clearConfirm();
        }
        _mirrorNames = names;
        _components.clear(); // mirror mode has no world-backed infos

        // Preserve selection across the rebuild by PATH (Qt::UserRole).
        const QString prevSelPath = _list->currentItem() ? _list->currentItem()->data(Qt::UserRole).toString() : QString();

        _list->blockSignals(true);
        _list->clear();
        QListWidgetItem* reselect = nullptr;
        for (const QString& path : names)
        {
            auto* item = new QListWidgetItem(componentLeaf(path), _list);
            item->setData(Qt::UserRole, path); // full path (string) → mirror mode
            if (path == prevSelPath)
            {
                reselect = item;
            }
        }
        _list->blockSignals(false);

        if (reselect)
        {
            _list->setCurrentItem(reselect);
        }
        else if (_list->count())
        {
            _list->setCurrentRow(0);
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
        _mirrorNames.clear();
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
