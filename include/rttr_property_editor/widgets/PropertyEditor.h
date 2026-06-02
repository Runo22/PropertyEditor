#pragma once

#include <QWidget>
#include <rttr/instance>
#include <rttr/type>
#include <rttr/variant>

class QTreeView;
class QLineEdit;
class QComboBox;
class QPushButton;
class QLabel;

namespace rpe {

class PropertyModel;
class PropertyDelegate;

enum class DisplayMode {
    ReadOnly,  // view is non-interactive, no toolbar editing controls
    EditLive,  // full editing with per-row override/reset
};

// Top-level widget: QTreeView + toolbar for displaying/editing RTTR properties.
class PropertyEditor : public QWidget
{
    Q_OBJECT

public:
    explicit PropertyEditor(QWidget* parent = nullptr);

    // ── Schema ────────────────────────────────────────────────────────────────
    void bindType(rttr::type type);
    void unbind();

    // ── Data ─────────────────────────────────────────────────────────────────
    // Main-thread: re-reads all properties from a live rttr::instance.
    void refresh(const rttr::instance& obj);

    // Thread-safe: inject a single value by dot-separated path.
    void setPropertyValue(const QString& path, rttr::variant val);

    // ── Mode ─────────────────────────────────────────────────────────────────
    void setMode(DisplayMode mode);
    DisplayMode mode() const { return _mode; }

signals:
    // Emitted when user commits an edit.
    void propertyEdited(const QString& path, const rttr::variant& newValue);

private slots:
    void _onFilterChanged(const QString& text);
    void _onModeChanged(int index);
    void _onResetAll();
    void _onContextMenu(const QPoint& pos);

private:
    void _setupUi();
    void _applyFilter(const QString& text, const QModelIndex& parent = {});

    PropertyModel*    _model      = nullptr;
    PropertyDelegate* _delegate   = nullptr;
    QTreeView*        _view       = nullptr;
    QLineEdit*        _filterEdit = nullptr;
    QComboBox*        _modeCombo  = nullptr;
    QPushButton*      _resetAllBtn= nullptr;
    DisplayMode       _mode       = DisplayMode::EditLive;
};

} // namespace rpe
