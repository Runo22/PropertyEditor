#pragma once

#include <QColor>
#include <QString>
#include <QWidget>

class QLineEdit;
class QToolButton;
class QLabel;

namespace rpe {

// ─────────────────────────────────────────────────────────────────────────────
//  FilePathEditor — line edit + "…" button. Used for file/dir path properties
//  (driven by the rpe::editor::FilePath / SaveFile / Directory hints).
// ─────────────────────────────────────────────────────────────────────────────
class FilePathEditor : public QWidget
{
    Q_OBJECT
    Q_PROPERTY(QString path READ path WRITE setPath NOTIFY pathChanged USER true)
public:
    enum class Mode { OpenFile, SaveFile, Directory };

    explicit FilePathEditor(Mode mode, QWidget* parent = nullptr);

    QString path() const;
    void    setPath(const QString& p);
    void    setFilter(const QString& f) { _filter = f; }

signals:
    void pathChanged();

private slots:
    void _browse();

private:
    Mode         _mode;
    QString      _filter = QStringLiteral("All files (*)");
    QLineEdit*   _edit   = nullptr;
    QToolButton* _button = nullptr;
};

// ─────────────────────────────────────────────────────────────────────────────
//  ColorEditor — swatch button that opens a QColorDialog. Used for QColor
//  properties (auto-detected) or the rpe::editor::Color hint.
// ─────────────────────────────────────────────────────────────────────────────
class ColorEditor : public QWidget
{
    Q_OBJECT
    Q_PROPERTY(QColor color READ color WRITE setColor NOTIFY colorChanged USER true)
public:
    explicit ColorEditor(QWidget* parent = nullptr);

    QColor color() const { return _color; }
    void   setColor(const QColor& c);

signals:
    void colorChanged();

private slots:
    void _pick();

private:
    void    _updateSwatch();
    QColor       _color = Qt::white;
    QLabel*      _swatch = nullptr;
    QToolButton* _button = nullptr;
};

} // namespace rpe
