#include "rpe/gui/PropertyDelegate.h"

#include "rpe/core/TypeRenderer.h"
#include "rpe/gui/PropertyModel.h"
#include "rpe/gui/VariantEditorFactory.h"

#include <QApplication>
#include <QPainter>
#include <QStyle>

namespace rpe
{

    namespace
    {

        // Collect the optional numeric metadata roles (and the flags flag) the value
        // editors honour, so the shared factory can apply min/max/step/decimals.
        varedit::EditorHints hintsFromIndex(const QModelIndex& i)
        {
            varedit::EditorHints h;
            if (const QVariant v = i.data(MinRole); v.isValid())
            {
                h.min = v.toDouble();
            }
            if (const QVariant v = i.data(MaxRole); v.isValid())
            {
                h.max = v.toDouble();
            }
            if (const QVariant v = i.data(StepRole); v.isValid())
            {
                h.step = v.toDouble();
            }
            if (const QVariant v = i.data(DecimalsRole); v.isValid())
            {
                h.decimals = v.toInt();
            }
            h.flags = i.data(FlagsRole).toBool();
            return h;
        }

    } // namespace

    PropertyDelegate::PropertyDelegate(PropertyModel* model, QObject* parent)
        : QStyledItemDelegate(parent)
        , _model(model)
    {
    }

    void PropertyDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const
    {
        // While an inline editor is open over this value cell, paint ONLY the item
        // background — not the value text/decoration. Otherwise the previous value
        // (e.g. "true") bleeds through editors that don't fully fill the cell, most
        // visibly a QCheckBox (which paints just its indicator) or a QColor swatch.
        // The editor widget itself sits on top of this background.
        if (!_editPath.isEmpty() && index.data(PropertyPathRole).toString() == _editPath)
        {
            QStyleOptionViewItem opt(option);
            initStyleOption(&opt, index);
            opt.text.clear();
            opt.icon = QIcon();
            opt.features &= ~(QStyleOptionViewItem::HasDisplay | QStyleOptionViewItem::HasDecoration);
            const QWidget* w = opt.widget;
            QStyle* style = w ? w->style() : QApplication::style();
            style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, w);
            return;
        }
        QStyledItemDelegate::paint(painter, option, index);
    }

    // Builds the editor widget for a leaf of declared type `t` (with editor hint
    // `ed`). Returns nullptr for types that aren't inline-editable. Kept separate so
    // createEditor only pins the row once it knows an editor will actually open.
    QWidget* PropertyDelegate::_makeEditor(rttr::type t, const QString& ed, const QModelIndex& index, QWidget* parent) const
    {
        return varedit::makeEditor(t, ed, hintsFromIndex(index), parent);
    }

    QWidget* PropertyDelegate::createEditor(QWidget* parent, const QStyleOptionViewItem&, const QModelIndex& index) const
    {
        if (!index.isValid())
        {
            return nullptr;
        }

        // Pick the editor from the node's DECLARED (schema) type, not the live value:
        // in mirror mode the value may not have arrived yet, and right after an edit
        // the live value is the editor's transient output — both would otherwise
        // select the wrong editor (e.g. a plain line edit for a path with no browse).
        const rttr::variant declared = index.data(DeclaredTypeRole).value<rttr::variant>();
        const rttr::type t = declared.is_valid()
            ? TypeRenderer::rawType(declared.get_value<rttr::type>())
            : TypeRenderer::rawType(index.data(RttrVariantRole).value<rttr::variant>().get_type());
        const QString ed = index.data(EditorHintRole).toString();

        QWidget* w = _makeEditor(t, ed, index, parent);
        if (!w)
        {
            return nullptr;
        }

        // Pin the row only now that an editor will actually open, so live refresh
        // can't clobber it — and a cell that yields no editor is never left stuck
        // stuck holding a local edit (which would freeze its live updates). Remember the prior pin
        // state so a cancelled edit can restore it.
        _editPath = index.data(PropertyPathRole).toString();
        _editHadLocalEdit = index.data(HasLocalEditRole).toBool();
        _editCommitted = false;
        _model->beginLocalEdit(_editPath);
        return w;
    }

    void PropertyDelegate::setEditorData(QWidget* editor, const QModelIndex& index) const
    {
        if (!editor || !index.isValid())
        {
            return;
        }
        varedit::setEditorData(editor, index.data(RttrVariantRole).value<rttr::variant>());
    }

    void PropertyDelegate::setModelData(QWidget* editor, QAbstractItemModel* model, const QModelIndex& index) const
    {
        if (!editor || !index.isValid())
        {
            return;
        }
        // Use the declared (schema) type, not the live value — the value may be
        // absent, or (right after an edit) hold the editor's transient output, either
        // of which would misroute the conversion below.
        const rttr::variant declared = index.data(DeclaredTypeRole).value<rttr::variant>();
        const rttr::type t = declared.is_valid()
            ? TypeRenderer::rawType(declared.get_value<rttr::type>())
            : TypeRenderer::rawType(index.data(RttrVariantRole).value<rttr::variant>().get_type());

        const rttr::variant newVal = varedit::readEditorData(editor, t);

        if (newVal.is_valid())
        {
            _editCommitted = true;
            model->setData(index, QVariant::fromValue(newVal), Qt::EditRole);
        }
    }

    void PropertyDelegate::updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& option, const QModelIndex&) const
    {
        editor->setGeometry(option.rect);
    }

    void PropertyDelegate::destroyEditor(QWidget* editor, const QModelIndex& index) const
    {
        // If the edit was cancelled (no commit) and the row was not pinned before we
        // opened the editor, release the implicit pin so live updates resume.
        if (!_editCommitted && !_editHadLocalEdit && !_editPath.isEmpty())
        {
            _model->resetNode(_editPath);
        }
        _editPath.clear();
        QStyledItemDelegate::destroyEditor(editor, index);
    }

} // namespace rpe
