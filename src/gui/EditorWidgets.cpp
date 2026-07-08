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
        // A tidy outline folder glyph, drawn to match the inspector's other icons
        // (thin round-joined strokes in `ink`). Rendered large and scaled down by the
        // button so it stays crisp on hi-dpi. Used as the "Browse…" affordance.
        QIcon folderIcon(const QColor& ink)
        {
            const int s = 40;
            QImage img(s, s, QImage::Format_ARGB32);
            img.fill(Qt::transparent);
            QPainter p(&img);
            p.setRenderHint(QPainter::Antialiasing, true);
            QPen pen(ink);
            pen.setWidthF(s * 0.075);
            pen.setCapStyle(Qt::RoundCap);
            pen.setJoinStyle(Qt::RoundJoin);
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);
            // Folder: a raised tab on the left, then the wider lid/body.
            QPainterPath path;
            path.moveTo(s * 0.13, s * 0.35);
            path.lineTo(s * 0.34, s * 0.35);
            path.lineTo(s * 0.42, s * 0.45);
            path.lineTo(s * 0.87, s * 0.45);
            path.lineTo(s * 0.87, s * 0.74);
            path.lineTo(s * 0.13, s * 0.74);
            path.closeSubpath();
            p.drawPath(path);
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
        _button = new QToolButton(this);
        _button->setIcon(folderIcon(palette().color(QPalette::ButtonText)));
        _button->setIconSize(QSize(16, 16));
        _button->setAutoRaise(true);
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
