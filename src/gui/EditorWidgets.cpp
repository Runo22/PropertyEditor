#include "rpe/gui/EditorWidgets.h"

#include <QColorDialog>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
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
        QIcon folderIcon()
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
        _button->setIconSize(QSize(22, 22));
        _button->setAutoRaise(true);
        _button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        _button->setStyleSheet(QStringLiteral("QToolButton { border: none; margin: 0; padding: 0; background: transparent; }"));
        _button->setToolTip(tr("Browse…"));
        _button->setCursor(Qt::ArrowCursor);
        _button->setFocusPolicy(Qt::NoFocus);
        layout->addWidget(_button);

        setFocusProxy(_edit);
        connect(_edit, &QLineEdit::textChanged, this, &FilePathEditor::pathChanged);
        connect(_button, &QToolButton::clicked, this, &FilePathEditor::_browse);
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
        }
    }

    void FilePathEditor::_browse()
    {
        QString picked;
        switch (_mode)
        {
        case Mode::OpenFile:
            picked = QFileDialog::getOpenFileName(this, tr("Select File"), _edit->text(), _filter);
            break;
        case Mode::SaveFile:
            picked = QFileDialog::getSaveFileName(this, tr("Save File"), _edit->text(), _filter);
            break;
        case Mode::Directory:
            picked = QFileDialog::getExistingDirectory(this, tr("Select Folder"), _edit->text());
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
