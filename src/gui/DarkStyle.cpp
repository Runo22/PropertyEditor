#include "rpe/gui/DarkStyle.h"

namespace rpe
{

    QString darkStyleSheet()
    {
        // One palette, referenced throughout:
        //   window #232629 · panels #1b1e20 · lines #3a3f44 · text #d6d9dd
        //   accent #2f6fed · muted #9aa0a6
        return QStringLiteral(R"(
/* ── base ─────────────────────────────────────────────────────────────── */
QWidget {
    background: #232629;
    color: #d6d9dd;
}
QLabel { background: transparent; }
QToolTip {
    background: #1b1e20;
    color: #d6d9dd;
    border: 1px solid #3a3f44;
    padding: 4px 6px;
}

/* ── filter fields (entities / components / properties) ───────────────── */
QLineEdit {
    background: #1b1e20;
    border: 1px solid #3a3f44;
    border-radius: 6px;
    padding: 4px 8px;
    selection-background-color: #2f6fed;
    selection-color: #ffffff;
}
QLineEdit:focus { border: 1px solid #2f6fed; }

/* ── entity + component lists ─────────────────────────────────────────── */
QListWidget {
    background: #1b1e20;
    border: 1px solid #3a3f44;
    border-radius: 6px;
    padding: 4px;
    outline: 0;
}
QListWidget::item {
    padding: 5px 8px;
    margin: 1px 2px;
    border-radius: 4px;
}
QListWidget::item:hover { background: rgba(255, 255, 255, 0.06); }
QListWidget::item:selected {
    background: #2f6fed;
    color: #ffffff;
}

/* ── property browser tree ────────────────────────────────────────────── */
QTreeView {
    background: #1b1e20;
    alternate-background-color: #202327;
    border: 1px solid #3a3f44;
    border-radius: 6px;
    padding: 2px;
    outline: 0;
}
QTreeView::item {
    padding: 3px 4px;
    border-radius: 3px;
}
QTreeView::item:hover { background: rgba(255, 255, 255, 0.05); }
QTreeView::item:selected {
    background: #2f6fed;
    color: #ffffff;
}
/* Paint the indentation/branch column of a selected row with the SAME accent, so
   the tree (e.g. the add-component popup) doesn't show a lighter palette-blue tab
   beside the row highlight. */
QTreeView::branch:selected { background: #2f6fed; }

/* Inline editors live INSIDE the tree cell — strip the field chrome (border,
   radius, big padding) so the text isn't clipped in the narrow value column. */
QTreeView QLineEdit,
QTreeView QComboBox,
QTreeView QAbstractSpinBox,
QTreeView QPlainTextEdit {
    border: none;
    border-radius: 0;
    padding: 0px 2px;
}
QHeaderView::section {
    background: #2a2e32;
    color: #b8bdc4;
    padding: 5px 8px;
    border: none;
    border-right: 1px solid #3a3f44;
    border-bottom: 1px solid #3a3f44;
}

/* ── buttons ──────────────────────────────────────────────────────────── */
/* Keep icon/text padding here so a custom QSS doesn't collapse the spacing
   and misalign the icon (that is the usual cause of the "+ Add" / menu icon
   drift). */
QToolButton {
    background: transparent;
    border: 1px solid transparent;
    border-radius: 5px;
    padding: 3px 8px;
}
QToolButton:hover {
    background: rgba(255, 255, 255, 0.08);
    border: 1px solid #3a3f44;
}
QToolButton:pressed { background: rgba(255, 255, 255, 0.04); }
QToolButton::menu-indicator { image: none; }

QPushButton {
    background: #2a2e32;
    border: 1px solid #3a3f44;
    border-radius: 6px;
    padding: 4px 12px;
}
QPushButton:hover { background: #333940; }
QPushButton:pressed { background: #23272b; }

/* ── file / folder popup menu ─────────────────────────────────────────── */
QMenu {
    background: #26292d;
    border: 1px solid #3a3f44;
    border-radius: 6px;
    padding: 4px;
}
/* Tight vertical padding so the row hugs the icon (a tall row makes the icon look
   small); left padding leaves an aligned icon column, right padding gives the
   label room. Both rows share this, so the file/folder icons line up. */
QMenu::item {
    padding: 4px 18px 4px 6px;
    border-radius: 4px;
}
QMenu::item:selected {
    background: #2f6fed;
    color: #ffffff;
}
QMenu::separator {
    height: 1px;
    background: #3a3f44;
    margin: 4px 6px;
}

/* ── add-component popup ──────────────────────────────────────────────── */
/* A square panel (no radius) with a matching background, and a flush inner tree,
   so no rounded inner corner shows the panel colour behind it. */
QFrame#rpeAddPopup {
    background: #1b1e20;
    border: 1px solid #3a3f44;
    border-radius: 0;
}
QFrame#rpeAddPopup QTreeWidget {
    background: transparent;
    border: none;
    border-radius: 0;
    padding: 0;
}

/* ── scrollbars ───────────────────────────────────────────────────────── */
QScrollBar:vertical {
    background: transparent;
    width: 10px;
    margin: 2px;
}
QScrollBar::handle:vertical {
    background: #3a3f44;
    border-radius: 5px;
    min-height: 24px;
}
QScrollBar::handle:vertical:hover { background: #4a5058; }
QScrollBar:horizontal {
    background: transparent;
    height: 10px;
    margin: 2px;
}
QScrollBar::handle:horizontal {
    background: #3a3f44;
    border-radius: 5px;
    min-width: 24px;
}
QScrollBar::handle:horizontal:hover { background: #4a5058; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }

/* ── splitter handle (Wide/Vertical layouts) ─────────────────────────── */
QSplitter::handle { background: #3a3f44; }
QSplitter::handle:horizontal { width: 2px; }
QSplitter::handle:vertical { height: 2px; }
)");
    }

} // namespace rpe
