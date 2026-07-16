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
    // Applying it is optional — nothing else in the library depends on it, so a
    // host with its own stylesheet can simply not call this.
    QString darkStyleSheet();

} // namespace rpe
