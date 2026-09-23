#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  Keyboard driving for the browser's search-then-pick surfaces.
//
//  Two shapes exist, and both must feel the same: you type, the best match is
//  already highlighted, the arrows walk the matches, Enter takes the highlighted
//  one. Focus never leaves the search box, so typing continues at any point.
//
//    • the "Add" PICKERS  — a filter box over a QTreeWidget of options in a
//      popup (spawnable prefabs, addable components) → driveTree().
//    • the panel FILTERS  — a filter box over the panel's own QListWidget
//      (entities, components) → driveList().
//
//  Kept private to src/ecs: this is how the widgets wire themselves up, not API.
// ─────────────────────────────────────────────────────────────────────────────

#include <QEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QList>
#include <QListWidget>
#include <QObject>
#include <QTreeWidget>
#include <QWidget>

#include <functional>

namespace rpe
{
    namespace picker
    {
        // Forwards key presses to a callback. Lets the filter box drive the view
        // below it without handing over focus. Plain QObject — no moc needed.
        class KeyRouter : public QObject
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

        // Is this row something the user can actually pick? Group headers and the
        // "(nothing available)" placeholder are neither selectable nor payload-
        // carrying, so one test covers every picker shape — grouped trees (options
        // are children) and flat ones (options are top level) alike.
        inline bool isOption(const QTreeWidgetItem* item)
        {
            return item && !item->isHidden() && (item->flags() & Qt::ItemIsSelectable)
                && (item->flags() & Qt::ItemIsEnabled) && !item->data(0, Qt::UserRole).isNull();
        }

        // Every visible option, top to bottom: the sequence the arrows walk and
        // Enter picks from. A hidden (filtered-out) group takes its children with it.
        inline void collectOptions(QTreeWidgetItem* parent, QList<QTreeWidgetItem*>& out)
        {
            for (int i = 0; i < parent->childCount(); ++i)
            {
                QTreeWidgetItem* c = parent->child(i);
                if (c->isHidden())
                {
                    continue;
                }
                if (isOption(c))
                {
                    out.append(c);
                }
                collectOptions(c, out);
            }
        }

        inline QList<QTreeWidgetItem*> options(QTreeWidget* tree)
        {
            QList<QTreeWidgetItem*> out;
            if (tree)
            {
                collectOptions(tree->invisibleRootItem(), out);
            }
            return out;
        }

        // Highlight the best (topmost visible) option, so a bare Enter right after
        // typing picks it. Call after every filter pass.
        inline void highlightFirst(QTreeWidget* tree)
        {
            const QList<QTreeWidgetItem*> opts = options(tree);
            tree->setCurrentItem(opts.isEmpty() ? nullptr : opts.first());
            if (!opts.isEmpty())
            {
                tree->scrollToItem(opts.first());
            }
        }

        // How many options a page-step should jump: what fits in the viewport.
        inline int pageStep(QTreeWidget* tree)
        {
            const int row = tree->sizeHintForRow(0);
            return row > 0 ? qMax(1, tree->viewport()->height() / row) : 10;
        }

        // Wire `search` to drive `tree`, and close `popup` on Escape.
        //
        //   Down/Up          walk the options, clamped at the ends
        //   Tab/Shift+Tab    walk them too, wrapping around (there is nowhere else
        //                    in a picker for Tab to go, and cycling is what a
        //                    filter-then-pick surface is for)
        //   PageDown/PageUp  jump a viewport-worth
        //   Enter            take the highlighted option — or the topmost one, so
        //                    Enter straight after typing picks the best match
        //   Esc              close without picking
        //
        // Home/End are deliberately left to the line edit, where they move the
        // text caret.
        //
        // Also re-highlights the topmost option whenever the text changes. Connect
        // your own filtering to textChanged FIRST — slots run in connection order,
        // so the highlight then lands on the freshly filtered list.
        inline void driveTree(QLineEdit* search, QTreeWidget* tree, QWidget* popup,
                              std::function<void(QTreeWidgetItem*)> activate)
        {
            QObject::connect(search, &QLineEdit::textChanged, tree, [tree] { highlightFirst(tree); });

            auto* keys = new KeyRouter(search);
            keys->onKey = [tree, popup, activate](QKeyEvent* ke) -> bool {
                const QList<QTreeWidgetItem*> opts = options(tree);
                const int cur = opts.indexOf(tree->currentItem());
                auto moveTo = [&](int idx, bool wrap) {
                    if (opts.isEmpty())
                    {
                        return true;
                    }
                    idx = wrap ? ((idx % opts.size()) + opts.size()) % opts.size() : qBound(0, idx, opts.size() - 1);
                    tree->setCurrentItem(opts[idx]);
                    tree->scrollToItem(opts[idx]);
                    return true;
                };

                switch (ke->key())
                {
                case Qt::Key_Down:
                    return moveTo(cur + 1, false);
                case Qt::Key_Up:
                    return moveTo(cur - 1, false);
                case Qt::Key_Tab:
                    return moveTo(cur + 1, true);
                case Qt::Key_Backtab:
                    return moveTo(cur - 1, true);
                case Qt::Key_PageDown:
                    return moveTo(cur + pageStep(tree), false);
                case Qt::Key_PageUp:
                    return moveTo(qMax(cur, 0) - pageStep(tree), false);
                case Qt::Key_Return:
                case Qt::Key_Enter:
                {
                    QTreeWidgetItem* pick = cur >= 0 ? opts[cur] : (opts.isEmpty() ? nullptr : opts.first());
                    if (pick)
                    {
                        activate(pick);
                    }
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

            highlightFirst(tree); // so a bare Enter picks the top option
        }

        // The panel filters' counterpart: `search` drives the list below it.
        //
        //   Down/Up          walk the visible rows (which selects them — selection
        //                    IS the action in these panels)
        //   PageDown/PageUp  jump a viewport-worth
        //   Enter            move focus into the list, to carry on there
        //   Esc              clear the filter
        //
        // Tab is left alone: in a panel it has somewhere to go (the list itself, per
        // the tab order), unlike in a picker popup.
        inline void driveList(QLineEdit* search, QListWidget* list)
        {
            auto* keys = new KeyRouter(search);
            keys->onKey = [search, list](QKeyEvent* ke) -> bool {
                QList<int> rows;
                for (int i = 0; i < list->count(); ++i)
                {
                    QListWidgetItem* it = list->item(i);
                    if (!it->isHidden() && (it->flags() & Qt::ItemIsSelectable) && (it->flags() & Qt::ItemIsEnabled))
                    {
                        rows.append(i);
                    }
                }
                const int cur = rows.indexOf(list->currentRow());
                auto moveTo = [&](int idx) {
                    if (rows.isEmpty())
                    {
                        return true;
                    }
                    idx = qBound(0, idx, rows.size() - 1);
                    list->setCurrentRow(rows[idx]);
                    list->scrollToItem(list->item(rows[idx]));
                    return true;
                };
                const int rowH = list->sizeHintForRow(0);
                const int page = rowH > 0 ? qMax(1, list->viewport()->height() / rowH) : 10;

                switch (ke->key())
                {
                case Qt::Key_Down:
                    return moveTo(cur + 1);
                case Qt::Key_Up:
                    return moveTo(cur - 1);
                case Qt::Key_PageDown:
                    return moveTo(cur + page);
                case Qt::Key_PageUp:
                    return moveTo(qMax(cur, 0) - page);
                case Qt::Key_Return:
                case Qt::Key_Enter:
                    if (!rows.isEmpty())
                    {
                        if (cur < 0)
                        {
                            list->setCurrentRow(rows.first());
                        }
                        list->setFocus(Qt::TabFocusReason);
                    }
                    return true;
                case Qt::Key_Escape:
                    search->clear(); // back to the unfiltered panel
                    return true;
                default:
                    return false;
                }
            };
            search->installEventFilter(keys);
        }

    } // namespace picker
} // namespace rpe
