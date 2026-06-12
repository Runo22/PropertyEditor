#include "rpe/gui/EditorWidgets.h"

#include <QColorDialog>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QToolButton>

namespace rpe
{

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

        _button = new QToolButton(this);
        _button->setText(QStringLiteral("…"));
        _button->setCursor(Qt::ArrowCursor);
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
