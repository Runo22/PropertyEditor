#include "rpe/gui/VariantEditor.h"

#include "rpe/gui/PropertyEditor.h"

#include <QLabel>
#include <QVBoxLayout>

namespace rpe {

VariantEditor::VariantEditor(QWidget* parent)
    : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    _typeLabel = new QLabel(tr("<no value>"), this);
    _typeLabel->setStyleSheet(QStringLiteral("font-weight: bold; padding: 2px 4px;"));
    layout->addWidget(_typeLabel);

    _editor = new PropertyEditor(this);
    _editor->setEditPolicy(EditPolicy::WriteBack);
    _editor->setInstanceProvider([this] { return _wrapper.instance(); });
    layout->addWidget(_editor, 1);

    connect(_editor, &PropertyEditor::propertyEdited, this,
            [this](const QString& path, const rttr::variant& v) {
                // The write already landed in _wrapper. Refresh dependent rows
                // (parent struct summaries, array siblings) on the next event-loop
                // turn: doing it synchronously would re-enter the model while
                // setData is still on the stack, and an array-size change would
                // then nest beginInsertRows inside the active edit.
                QMetaObject::invokeMethod(this, [this] { refreshFromSource(); },
                                          Qt::QueuedConnection);
                emit valueChanged(path, v);
            });
}

void VariantEditor::setVariant(const rttr::variant& v)
{
    _wrapper = RttrVariantWrapper(v);
    _rebind();
}

void VariantEditor::setLinked(rttr::type type, void* obj)
{
    _wrapper = RttrVariantWrapper::makeLinked(type, obj);
    _rebind();
}

void VariantEditor::clear()
{
    _wrapper.clear();
    _editor->unbind();
    _typeLabel->setText(tr("<no value>"));
}

void VariantEditor::_rebind()
{
    if (!_wrapper.isValid()) { clear(); return; }
    _typeLabel->setText(_wrapper.typeName());
    _editor->bindType(_wrapper.type());
    _editor->refresh(_wrapper.instance());
    _editor->expandAll();
}

void VariantEditor::refreshFromSource()
{
    if (_wrapper.isValid())
        _editor->refresh(_wrapper.instance());
}

bool VariantEditor::isReadOnly() const     { return _editor->isReadOnly(); }
void VariantEditor::setReadOnly(bool ro)   { _editor->setReadOnly(ro); }

} // namespace rpe
