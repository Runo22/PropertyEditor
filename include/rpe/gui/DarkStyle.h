#pragma once

#include <QString>

namespace rpe
{

    // A self-contained dark stylesheet for the inspector widgets — entity/component
    // lists, the property browser tree, filter fields, buttons and the file/folder
    // menu. Rounded "shape" controls with generous padding for legibility.
    //
    // Apply it to an EntityComponentBrowser / PropertyEditor (it cascades to the
    // children) or to qApp:
    //     browser.setStyleSheet(rpe::darkStyleSheet());
    //
    // Entirely optional and self-contained (this file + src/gui/DarkStyle.cpp) so it
    // can be dropped without touching anything else.
    QString darkStyleSheet();

} // namespace rpe
