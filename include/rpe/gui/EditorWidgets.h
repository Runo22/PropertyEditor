#pragma once

#include <QColor>
#include <QComboBox>
#include <QList>
#include <QPair>
#include <QString>
#include <QWidget>

#include <cstdint>

class QLineEdit;
class QToolButton;
class QLabel;
class QStandardItemModel;

namespace rpe
{

    // ─────────────────────────────────────────────────────────────────────────────
    //  FilePathEditor — line edit + "…" button. Used for file/dir path properties
    //  (driven by the rpe::editor::FilePath / SaveFile / Directory hints).
    // ─────────────────────────────────────────────────────────────────────────────
    class FilePathEditor : public QWidget
    {
        Q_OBJECT
        Q_PROPERTY(QString path READ path WRITE setPath NOTIFY pathChanged USER true)

    public:
        enum class Mode
        {
            OpenFile,
            SaveFile,
            Directory,
            FileOrDirectory // browse button offers BOTH a file and a folder picker
        };

        explicit FilePathEditor(Mode mode, QWidget* parent = nullptr);

        QString path() const;
        void setPath(const QString& p);
        void setFilter(const QString& f)
        {
            _filter = f;
        }

    signals:
        void pathChanged();

    private slots:
        void _browse();

    private:
        void _pick(Mode m); // run the dialog for a specific mode, store the result
        Mode _mode;
        QString _filter = QStringLiteral("All files (*)");
        QLineEdit* _edit = nullptr;
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

        QColor color() const
        {
            return _color;
        }
        void setColor(const QColor& c);

    signals:
        void colorChanged();

    private slots:
        void _pick();

    private:
        void _updateSwatch();
        QColor _color = Qt::white;
        QLabel* _swatch = nullptr;
        QToolButton* _button = nullptr;
    };

    // ─────────────────────────────────────────────────────────────────────────────
    //  FlagsEditor — a checkable combo box for bitmask (flags) enum properties.
    //
    //  One checkable row per named flag; the collapsed line shows the combined
    //  value ("Fire | Poison"). Clicking a row toggles its bit without closing the
    //  popup. The exposed `bits` is the OR of the checked flags; the delegate turns
    //  it back into the exact enum type via FlagsBridge. The zero-valued entry (if
    //  any) is not a checkbox — clearing every box IS the zero value.
    // ─────────────────────────────────────────────────────────────────────────────
    class FlagsEditor : public QComboBox
    {
        Q_OBJECT
        Q_PROPERTY(qlonglong bits READ bits WRITE setBits NOTIFY bitsChanged USER true)

    public:
        explicit FlagsEditor(QWidget* parent = nullptr);

        // Define the selectable flags (display name + its bit value). Entries whose
        // value is 0 are ignored (clearing all boxes already means zero).
        void setFlags(const QList<QPair<QString, qint64>>& flags);

        qlonglong bits() const;
        void setBits(qlonglong bits);

    signals:
        void bitsChanged();

    protected:
        bool eventFilter(QObject* obj, QEvent* ev) override;

    private:
        void _refreshText();
        QStandardItemModel* _model = nullptr;
        bool _guard = false; // suppress reentrant updates while we mutate the model
    };

} // namespace rpe
