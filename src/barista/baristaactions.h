#pragma once

#include <QObject>
#include <QVariantMap>
#include <QVariantList>
#include <optional>

class Settings;
class MachineState;

// [barista-fork] Apply-on-confirm engine. When the assistant recommends a change (via the app's
// structuredNext fenced-JSON contract) and the user confirms, this applies it to the next-shot dial:
// dose / yield / temperature are written immediately; the GRINDER is off-machine, so it goes to a
// pending queue and is reminded + confirmed at the next shot. Confirm-always, never actuates the
// machine. Exposed to QML as Barista.actions.
class BaristaActions : public QObject {
    Q_OBJECT
public:
    BaristaActions(Settings* settings, MachineState* machineState, QObject* parent = nullptr);

    // Apply a structuredNext map. doseG/targetWeightG/temperatureC apply now; grinderSetting queues.
    // Returns { applied: [str], queued: [str], blocked: bool, blockedReason: str }.
    Q_INVOKABLE QVariantMap applyFromNext(const QVariantMap& next, qint64 anchorShotId = 0);
    Q_INVOKABLE void undoLast();
    Q_INVOKABLE bool canUndo() const { return !m_undo.isEmpty(); }

    // Conservative yes/no from a spoken reply: 1 = yes, 0 = no, -1 = neither (pass through to the AI).
    Q_INVOKABLE int parseConfirmation(const QString& reply) const;

    // Off-machine grinder queue.
    Q_INVOKABLE QVariantMap outstandingGrind();          // newest pending setGrinder, or {}
    Q_INVOKABLE void enqueueGrind(const QString& value, qint64 anchorShotId = 0);
    Q_INVOKABLE void resolveGrind(bool done);            // done → write dyeGrinderSetting; else decline

    static std::optional<bool> parseConfirmationReply(const QString& reply);

private:
    QVariantList loadPending() const;
    void savePending(const QVariantList& list);
    void expireStale(QVariantList& list) const;

    Settings* m_settings = nullptr;
    MachineState* m_machine = nullptr;
    QVariantMap m_undo;   // prior values, for undoLast()
};
