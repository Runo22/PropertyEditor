#include "rpe/gui/PropertyDelegate.h"

#include "rpe/core/TypeRenderer.h"
#include "rpe/gui/EditorWidgets.h"
#include "rpe/gui/PropertyModel.h"
#include "rpe/gui/VariantEditorFactory.h"

#include <QAbstractProxyModel>
#include <QApplication>
#include <QPainter>
#include <QPen>
#include <QStyle>

#include <cmath>
#include <cstdint>

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

    namespace
    {
        int lockSide(const QStyleOptionViewItem& option)
        {
            return qBound(8, option.rect.height() * 11 / 20, 14);
        }
    } // namespace

    int readOnlyLockWidth(const QStyleOptionViewItem& option)
    {
        return lockSide(option) + 6;
    }

    void paintReadOnlyLock(QPainter* painter, const QStyleOptionViewItem& option)
    {
        // A shackle over a body, in the text colour, faint — present, not loud.
        const int side = lockSide(option);
        const bool selected = option.state & QStyle::State_Selected;
        QColor c = option.palette.color(selected ? QPalette::HighlightedText : QPalette::Text);
        c.setAlphaF(selected ? 0.75 : 0.40);
        const QRectF box(option.rect.right() - readOnlyLockWidth(option) + 3, option.rect.center().y() - side / 2.0,
                         side, side);
        const QRectF body(box.left(), box.top() + box.height() * 0.45, box.width(), box.height() * 0.55);
        const qreal sw = box.width() * 0.56;
        const QRectF shackle(box.center().x() - sw / 2, box.top(), sw, box.height() * 0.80);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setPen(QPen(c, qMax<qreal>(1.2, side / 9.0)));
        painter->setBrush(Qt::NoBrush);
        painter->drawArc(shackle, 0, 180 * 16);
        painter->drawLine(QPointF(shackle.left(), shackle.center().y()), QPointF(shackle.left(), body.top()));
        painter->drawLine(QPointF(shackle.right(), shackle.center().y()), QPointF(shackle.right(), body.top()));
        painter->setPen(Qt::NoPen);
        painter->setBrush(c);
        painter->drawRoundedRect(body, 1.5, 1.5);
        painter->restore();
    }

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

        // A read-only value keeps its NORMAL look — it is information, not a
        // disabled control — plus a small, faint lock at the right edge that says
        // "deliberately not editable" (the reason is in the tooltip).
        if (index.column() == 1 && !index.data(ReadOnlyRole).toString().isEmpty())
        {
            const int lockW = readOnlyLockWidth(option);

            // Background/selection across the WHOLE cell, so the highlight doesn't
            // stop short of the lock...
            QStyleOptionViewItem bg(option);
            initStyleOption(&bg, index);
            bg.text.clear();
            bg.icon = QIcon();
            bg.features &= ~(QStyleOptionViewItem::HasDisplay | QStyleOptionViewItem::HasDecoration);
            const QWidget* w = bg.widget;
            QStyle* style = w ? w->style() : QApplication::style();
            style->drawControl(QStyle::CE_ItemViewItem, &bg, painter, w);

            // ...the value itself in the remaining width (elided, never under the lock)...
            QStyleOptionViewItem text(option);
            text.rect.setRight(option.rect.right() - lockW);
            QStyledItemDelegate::paint(painter, text, index);

            // ...and the lock.
            paintReadOnlyLock(painter, option);
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
        if (index.data(InlineVectorRole).toBool())
        {
            // A vector row is NOT pinned: its fields keep updating underneath, and
            // on commit only the fields the user changed are written (each through
            // its own path), so nothing else is frozen or overwritten.
            _editPath.clear();
            return _makeVectorEditor(index, parent);
        }

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
        if (auto* ve = qobject_cast<VectorEditor*>(editor))
        {
            _setVectorData(ve, index);
            return;
        }
        varedit::setEditorData(editor, index.data(RttrVariantRole).value<rttr::variant>());
        const rttr::variant declared = index.data(DeclaredTypeRole).value<rttr::variant>();
        varedit::rememberOpeningValue(editor,
                                      declared.is_valid()
                                          ? TypeRenderer::rawType(declared.get_value<rttr::type>())
                                          : TypeRenderer::rawType(index.data(RttrVariantRole).value<rttr::variant>().get_type()));
    }

    void PropertyDelegate::setModelData(QWidget* editor, QAbstractItemModel* model, const QModelIndex& index) const
    {
        if (!editor || !index.isValid())
        {
            return;
        }
        if (auto* ve = qobject_cast<VectorEditor*>(editor))
        {
            _commitVector(ve, index);
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

        // Left as it was opened → nothing to write. Treated as a cancel, so the row
        // goes back to following live values. (Writing it would, for a value outside
        // the editor's Min/Max, store the CLAMPED value the user never chose.)
        if (varedit::unchangedSinceOpen(editor, newVal))
        {
            return;
        }
        if (newVal.is_valid())
        {
            _editCommitted = true;
            model->setData(index, QVariant::fromValue(newVal), Qt::EditRole);
        }
    }

    namespace
    {
        // The source-model index for a (possibly proxied) view index — a vector's
        // fields are read and written on the SOURCE model, so a filter hiding some
        // field rows can't hide them from the editor.
        QModelIndex toSource(const QModelIndex& i)
        {
            if (const auto* p = qobject_cast<const QAbstractProxyModel*>(i.model()))
            {
                return p->mapToSource(i);
            }
            return i;
        }

        constexpr const char* kVectorOpening = "rpeVectorOpening";
    } // namespace

    QWidget* PropertyDelegate::_makeVectorEditor(const QModelIndex& index, QWidget* parent) const
    {
        const QModelIndex row = toSource(index).siblingAtColumn(0);
        QVector<VectorEditor::Field> fields;
        for (int r = 0; r < _model->rowCount(row); ++r)
        {
            const QModelIndex name = _model->index(r, 0, row);
            const QModelIndex value = _model->index(r, 1, row);
            VectorEditor::Field f;
            f.name = name.data(Qt::DisplayRole).toString();
            const rttr::variant declared = value.data(DeclaredTypeRole).value<rttr::variant>();
            const rttr::type t = declared.is_valid() ? TypeRenderer::rawType(declared.get_value<rttr::type>())
                                                     : rttr::type::get<double>();
            f.integral = t != rttr::type::get<float>() && t != rttr::type::get<double>()
                && t != rttr::type::get<long double>();
            if (const QVariant v = value.data(MinRole); v.isValid())
                f.min = v.toDouble();
            else if (t == rttr::type::get<unsigned char>() || t == rttr::type::get<unsigned short>())
                f.min = 0; // (wider unsigned types never make an inline vector)
            if (const QVariant v = value.data(MaxRole); v.isValid())
                f.max = v.toDouble();
            if (const QVariant v = value.data(StepRole); v.isValid())
                f.step = v.toDouble();
            if (const QVariant v = value.data(DecimalsRole); v.isValid())
                f.decimals = v.toInt();
            fields.append(f);
        }
        return new VectorEditor(fields, parent);
    }

    void PropertyDelegate::_setVectorData(VectorEditor* ve, const QModelIndex& index) const
    {
        // Fill ONCE, on opening. The fields keep streaming live underneath; the view
        // may call setEditorData again on a change, and that must not overwrite what
        // the user is typing.
        if (ve->property(kVectorOpening).isValid())
        {
            return;
        }
        const QModelIndex row = toSource(index).siblingAtColumn(0);
        QVariantList opening;
        for (int i = 0; i < ve->count(); ++i)
        {
            const rttr::variant v =
                TypeRenderer::unwrap(_model->index(i, 1, row).data(RttrVariantRole).value<rttr::variant>());
            if (v.is_valid())
            {
                ve->setValue(i, v.to_double());
            }
            opening << ve->value(i); // what the box SHOWS (after range clamping)
        }
        ve->setProperty(kVectorOpening, opening);
    }

    void PropertyDelegate::_commitVector(VectorEditor* ve, const QModelIndex& index) const
    {
        const QModelIndex row = toSource(index).siblingAtColumn(0);
        const QVariantList opening = ve->property(kVectorOpening).toList();
        for (int i = 0; i < ve->count(); ++i)
        {
            const double now = ve->value(i);
            if (i < opening.size() && opening[i].toDouble() == now)
            {
                continue; // untouched field: write nothing
            }
            const QModelIndex value = _model->index(i, 1, row);
            const rttr::variant declared = value.data(DeclaredTypeRole).value<rttr::variant>();
            const rttr::variant v = varedit::numberAs(
                now, declared.is_valid() ? declared.get_value<rttr::type>() : rttr::type::get<double>());
            if (v.is_valid())
            {
                _model->setData(value, QVariant::fromValue(v), Qt::EditRole);
            }
        }
        ve->setProperty(kVectorOpening, QVariant()); // committed; a reopen reads afresh
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
