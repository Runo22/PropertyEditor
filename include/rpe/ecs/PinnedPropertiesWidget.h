#pragma once

#include "rpe/core/rttr_prelude.h"

#include "rpe/ecs/MirrorChannel.h"

#include <QSet>
#include <QWidget>

#include <memory>

class QTreeWidget;
class QTreeWidgetItem;
class QTimer;

namespace rpe
{

    class EcsMirror;

    // ─────────────────────────────────────────────────────────────────────────────
    //  PinnedPropertiesWidget — a WATCH LIST of pinned properties, across any
    //  number of entities and components, each row showing the live value and
    //  (optionally) editing it in place.
    //
    //  Pinning is NOT a local edit: pinned values keep following the simulation;
    //  the pin only decides *what is shown here* (and tinted in the main tree).
    //
    //  Standalone on purpose: host it anywhere (e.g. its own dock page). Wire it
    //  to a browser with EntityComponentBrowser::setPinnedPropertiesWidget(), or
    //  drive it manually via setMirror()/setChannel() + pin().
    //
    //  Live values flow through the EcsMirror channel, so they require mirror
    //  mode; edits are queued to the simulation thread the same way the property
    //  editor's edits are.
    // ─────────────────────────────────────────────────────────────────────────────
    class PinnedPropertiesWidget : public QWidget
    {
        Q_OBJECT

    public:
        explicit PinnedPropertiesWidget(QWidget* parent = nullptr);

        // Share the producer's channel (values in, edits out). setMirror is a
        // convenience for setChannel(mirror->channel()).
        void setMirror(EcsMirror* mirror);
        void setChannel(std::shared_ptr<MirrorChannel> channel);

        QVector<MirrorChannel::PinKey> pins() const;
        bool isPinned(qulonglong entity, const QString& component, const QString& path) const;
        // Paths pinned for one entity+component — for tinting rows in a property tree.
        QSet<QString> pinnedPaths(qulonglong entity, const QString& component) const;

    public slots:
        void pin(qulonglong entity, const QString& entityLabel, const QString& component, const QString& path);
        void unpin(qulonglong entity, const QString& component, const QString& path);
        void clearPins();
        // One poll cycle (also runs on the internal ~30 Hz timer). Public so tests
        // and hosts can force an immediate refresh.
        void pollNow();

    signals:
        void pinsChanged();

    private slots:
        void _onItemChanged(QTreeWidgetItem* item, int column);
        void _onContextMenu(const QPoint& pos);

    private:
        void _setupUi();
        void _pushPins(); // send the current pin set to the channel
        QTreeWidgetItem* _findItem(const MirrorChannel::PinKey& key) const;
        static MirrorChannel::PinKey _itemKey(const QTreeWidgetItem* item);

        std::shared_ptr<MirrorChannel> _channel;
        QTreeWidget* _tree = nullptr;
        QTimer* _timer = nullptr;
        bool _updating = false; // guards itemChanged during programmatic updates
    };

} // namespace rpe
