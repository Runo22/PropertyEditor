#include "rpe/ecs/RowActions.h"

#include <QAction>
#include <QMenu>

namespace rpe
{

    namespace
    {
        // A menu title as the user reads it: '&' mnemonics removed, "&&" kept as '&'.
        QString plainTitle(const QString& title)
        {
            QString out;
            out.reserve(title.size());
            for (int i = 0; i < title.size(); ++i)
            {
                if (title[i] == QLatin1Char('&'))
                {
                    if (i + 1 < title.size() && title[i + 1] == QLatin1Char('&'))
                    {
                        out += QLatin1Char('&');
                        ++i;
                    }
                    continue;
                }
                out += title[i];
            }
            return out;
        }
    } // namespace

    QMenu* subMenu(QMenu& root, const QStringList& path)
    {
        QMenu* at = &root;
        for (const QString& raw : path)
        {
            const QString level = raw.trimmed();
            if (level.isEmpty())
            {
                continue;
            }
            const QString key = plainTitle(level);
            QMenu* found = nullptr;
            for (QAction* a : at->actions())
            {
                if (a->menu() && plainTitle(a->menu()->title()) == key)
                {
                    found = a->menu();
                    break;
                }
            }
            at = found ? found : at->addMenu(level);
        }
        return at;
    }

} // namespace rpe
