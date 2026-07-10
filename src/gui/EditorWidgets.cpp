#include "rpe/gui/EditorWidgets.h"

#include <QAction>
#include <QColorDialog>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QProxyStyle>
#include <QStyle>
#include <QToolButton>

namespace rpe
{

    namespace
    {
        // A filled Material-style folder glyph (amber). Reproduces this two-path SVG
        // bezier-for-bezier on a 48x48 viewBox — a darker back tab (#ffa000) with the
        // lighter body (#ffca28) on top — so no QtSvg dependency is needed:
        //   <path fill="#ffa000" d="M40,12H22l-4-4H8c-2.2,0-4,1.8-4,4v24c0,2.2,1.8,4,4,4
        //         h29.7L44,29V16C44,13.8,42.2,12,40,12z"/>
        //   <path fill="#ffca28" d="M40,12H8c-2.2,0-4,1.8-4,4v20c0,2.2,1.8,4,4,4h32c2.2,0,
        //         4-1.8,4-4V16C44,13.8,42.2,12,40,12z"/>
        // Rendered large and scaled down by the button so it stays crisp on hi-dpi.
        // Built once (the glyph is a constant) and shared.
        QIcon buildFolderIcon()
        {
            // Back tab (#ffa000).
            QPainterPath back;
            back.moveTo(40, 12);
            back.lineTo(22, 12);
            back.lineTo(18, 8);
            back.lineTo(8, 8);
            back.cubicTo(5.8, 8, 4, 9.8, 4, 12);
            back.lineTo(4, 36);
            back.cubicTo(4, 38.2, 5.8, 40, 8, 40);
            back.lineTo(37.7, 40);
            back.lineTo(44, 29);
            back.lineTo(44, 16);
            back.cubicTo(44, 13.8, 42.2, 12, 40, 12);
            back.closeSubpath();

            // Front body (#ffca28), drawn over the tab.
            QPainterPath front;
            front.moveTo(40, 12);
            front.lineTo(8, 12);
            front.cubicTo(5.8, 12, 4, 13.8, 4, 16);
            front.lineTo(4, 36);
            front.cubicTo(4, 38.2, 5.8, 40, 8, 40);
            front.lineTo(40, 40);
            front.cubicTo(42.2, 40, 44, 38.2, 44, 36);
            front.lineTo(44, 16);
            front.cubicTo(44, 13.8, 42.2, 12, 40, 12);
            front.closeSubpath();

            const int s = 96; // hi-res, downscaled by the button for crispness
            QImage img(s, s, QImage::Format_ARGB32);
            img.fill(Qt::transparent);
            QPainter p(&img);
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setPen(Qt::NoPen);

            // The SVG leaves whitespace inside its 48x48 box; scale the glyph's actual
            // bounds to fill the icon (tiny margin) so it isn't rendered small.
            const QRectF b = back.boundingRect().united(front.boundingRect());
            const qreal margin = s * 0.04;
            const qreal scale = (s - 2 * margin) / qMax(b.width(), b.height());
            p.translate(margin + (s - 2 * margin - b.width() * scale) / 2.0,
                        margin + (s - 2 * margin - b.height() * scale) / 2.0);
            p.scale(scale, scale);
            p.translate(-b.left(), -b.top());

            p.fillPath(back, QColor(0xFF, 0xA0, 0x00));
            p.fillPath(front, QColor(0xFF, 0xCA, 0x28));
            p.end();
            return QIcon(QPixmap::fromImage(img));
        }

        const QIcon& folderIcon()
        {
            static const QIcon icon = buildFolderIcon();
            return icon;
        }

        // A filled document glyph (blue-grey) with a folded corner, drawn the SAME way
        // as the folder — same 48 viewBox, same bounds-fill margin — so the two read as
        // a matched set and line up in the file/folder menu.
        QIcon buildFileIcon()
        {
            QPainterPath body; // page, with the top-right corner cut for the fold
            body.moveTo(12, 6);
            body.lineTo(30, 6);
            body.lineTo(38, 14);
            body.lineTo(38, 42);
            body.lineTo(12, 42);
            body.closeSubpath();

            QPainterPath fold; // the folded corner triangle
            fold.moveTo(30, 6);
            fold.lineTo(38, 14);
            fold.lineTo(30, 14);
            fold.closeSubpath();

            QPainterPath lines; // a few "text" rules
            lines.addRect(17, 22, 16, 2);
            lines.addRect(17, 28, 16, 2);
            lines.addRect(17, 34, 11, 2);

            const int s = 96;
            QImage img(s, s, QImage::Format_ARGB32);
            img.fill(Qt::transparent);
            QPainter p(&img);
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setPen(Qt::NoPen);

            const QRectF b = body.boundingRect();
            const qreal margin = s * 0.04;
            const qreal scale = (s - 2 * margin) / qMax(b.width(), b.height());
            p.translate(margin + (s - 2 * margin - b.width() * scale) / 2.0,
                        margin + (s - 2 * margin - b.height() * scale) / 2.0);
            p.scale(scale, scale);
            p.translate(-b.left(), -b.top());

            p.fillPath(body, QColor(0x90, 0xA4, 0xAE));  // blue-grey 300 page
            p.fillPath(fold, QColor(0x60, 0x7D, 0x8B));  // blue-grey 600 corner
            p.fillPath(lines, QColor(0xEC, 0xEF, 0xF1)); // near-white text
            p.end();
            return QIcon(QPixmap::fromImage(img));
        }

        const QIcon& fileIcon()
        {
            static const QIcon icon = buildFileIcon();
            return icon;
        }

        // Bumps the menu icon size (default ~16px looks small next to the label). A
        // proxy over the current style so any active QSS still renders the menu.
        class MenuIconStyle : public QProxyStyle
        {
        public:
            using QProxyStyle::QProxyStyle;
            int pixelMetric(PixelMetric m, const QStyleOption* opt = nullptr, const QWidget* w = nullptr) const override
            {
                if (m == QStyle::PM_SmallIconSize)
                    return 20;
                return QProxyStyle::pixelMetric(m, opt, w);
            }
        };
    } // namespace

    // ── FilePathEditor ─────────────────────────────────────────────────────────────

    FilePathEditor::FilePathEditor(Mode mode, QWidget* parent)
        : QWidget(parent)
        , _mode(mode)
    {
        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);

        _edit = new QLineEdit(this);
        _edit->setFrame(false);
        layout->addWidget(_edit, 1);

        // Icon-only, flat browse button — a folder glyph instead of a "…" label.
        // No border/padding so the icon fills the cell height instead of shrinking.
        _button = new QToolButton(this);
        _button->setIcon(folderIcon());
        _button->setIconSize(QSize(16, 16)); // compact enough to fit a tree-cell row
        _button->setAutoRaise(true);
        _button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        _button->setCursor(Qt::ArrowCursor);
        _button->setFocusPolicy(Qt::NoFocus);
        layout->addWidget(_button);

        if (_mode == Mode::FileOrDirectory)
        {
            // The type alone can't say file-vs-folder, so offer both from a tidy
            // popup on the same folder button (no separate split control). The menu
            // indicator arrow is hidden so the button stays a clean icon.
            _button->setStyleSheet(QStringLiteral(
                "QToolButton { border: none; margin: 0; padding: 0; background: transparent; }"
                "QToolButton::menu-indicator { image: none; }"));
            _button->setToolTip(tr("Browse for a file or folder…"));
            auto* menu = new QMenu(_button);
            auto* menuStyle = new MenuIconStyle; // wraps the app style; larger icons
            menuStyle->setParent(menu);
            menu->setStyle(menuStyle);
            QAction* pickFile = menu->addAction(fileIcon(), tr("Choose File…"));
            QAction* pickDir = menu->addAction(folderIcon(), tr("Choose Folder…"));
            connect(pickFile, &QAction::triggered, this, [this] { _pick(Mode::OpenFile); });
            connect(pickDir, &QAction::triggered, this, [this] { _pick(Mode::Directory); });
            _button->setMenu(menu);
            _button->setPopupMode(QToolButton::InstantPopup);
        }
        else
        {
            _button->setStyleSheet(QStringLiteral("QToolButton { border: none; margin: 0; padding: 0; background: transparent; }"));
            _button->setToolTip(tr("Browse…"));
            connect(_button, &QToolButton::clicked, this, &FilePathEditor::_browse);
        }

        setFocusProxy(_edit);
        connect(_edit, &QLineEdit::textChanged, this, &FilePathEditor::pathChanged);
    }

    QString FilePathEditor::path() const
    {
        return _edit->text();
    }

    void FilePathEditor::setPath(const QString& p)
    {
        if (_edit->text() != p)
        {
            _edit->setText(p);
            // Show the START of a long path (the meaningful part) rather than letting
            // it scroll to the end in a narrow value column.
            _edit->setCursorPosition(0);
        }
    }

    void FilePathEditor::_browse()
    {
        _pick(_mode);
    }

    void FilePathEditor::_pick(Mode m)
    {
        QString picked;
        switch (m)
        {
        case Mode::SaveFile:
            picked = QFileDialog::getSaveFileName(this, tr("Save File"), _edit->text(), _filter);
            break;
        case Mode::Directory:
            picked = QFileDialog::getExistingDirectory(this, tr("Select Folder"), _edit->text());
            break;
        case Mode::OpenFile:
        case Mode::FileOrDirectory: // FileOrDirectory drives _pick with a concrete mode
            picked = QFileDialog::getOpenFileName(this, tr("Select File"), _edit->text(), _filter);
            break;
        }
        if (!picked.isEmpty())
        {
            setPath(picked);
        }
    }

    // ── ColorEditor ────────────────────────────────────────────────────────────────

    ColorEditor::ColorEditor(QWidget* parent)
        : QWidget(parent)
    {
        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(2, 1, 0, 1);
        layout->setSpacing(4);

        _swatch = new QLabel(this);
        _swatch->setMinimumWidth(28);
        _swatch->setFrameShape(QFrame::StyledPanel);
        layout->addWidget(_swatch, 1);

        _button = new QToolButton(this);
        _button->setText(QStringLiteral("…"));
        layout->addWidget(_button);

        connect(_button, &QToolButton::clicked, this, &ColorEditor::_pick);
        _updateSwatch();
    }

    void ColorEditor::setColor(const QColor& c)
    {
        if (_color == c)
        {
            return;
        }
        _color = c;
        _updateSwatch();
        emit colorChanged();
    }

    void ColorEditor::_updateSwatch()
    {
        _swatch->setStyleSheet(QStringLiteral("background-color: %1; border: 1px solid #555;")
                                   .arg(_color.name(QColor::HexArgb)));
        _swatch->setText(_color.name());
    }

    void ColorEditor::_pick()
    {
        const QColor c = QColorDialog::getColor(_color, this, tr("Select Color"), QColorDialog::ShowAlphaChannel);
        if (c.isValid())
        {
            setColor(c);
        }
    }

} // namespace rpe
