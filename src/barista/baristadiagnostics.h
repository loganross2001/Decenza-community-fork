#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>
#include <QMutex>
#include <QStringList>
#include <QDateTime>

class QFile;

// [barista-fork] Always-on diagnostic recorder for the barista voice + coaching subsystem.
//
// Records a timestamped, human-readable TIMELINE of everything the barista does — sessions,
// model turns, tool calls, every speak() start/stop/INTERRUPT, live coach cues (and why they
// were suppressed), detector flags surfaced to the model, STT results, and mic gating — so the
// owner can reproduce a glitch (barista talking over itself, missing coaching, a spurious
// "skipped frame" warning) and hand the log back for a precise fix.
//
// Design notes:
//   * LOCAL ONLY. Nothing is ever sent anywhere — it is written to a file on the device and
//     read back by the owner. (Honours the owner's "nothing leaves unless I ask" rule.)
//   * THREAD-SAFE. Events arrive from the BLE sample thread (coaches), background DB threads
//     (tools), and the GUI thread (voice/overlay). All access is under one mutex.
//   * ZERO-SETUP call sites. Instrumentation across subsystems calls the static record(); it is
//     a no-op when disabled or before the instance exists, so it can be sprinkled anywhere.
//   * CRASH-SAFE. Each event is appended to the log file and flushed immediately, so a hang or
//     crash still leaves the timeline up to the last event on disk.
class BaristaDiagnostics : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)
    Q_PROPERTY(int eventCount READ eventCount NOTIFY eventCountChanged)
    Q_PROPERTY(QString logFilePath READ logFilePath CONSTANT)

public:
    explicit BaristaDiagnostics(QObject* parent = nullptr);
    ~BaristaDiagnostics() override;

    // Static entry point for C++ call sites in any subsystem. No-op if there is no live instance
    // or logging is disabled. `detail` is rendered as space-separated key=value pairs.
    static void record(const QString& category, const QString& event,
                       const QVariantMap& detail = QVariantMap());

    bool enabled() const;
    void setEnabled(bool on);
    int eventCount() const;
    QString logFilePath() const { return m_filePath; }

    // QML-callable (the overlay logs UI-side turn events through mark()).
    Q_INVOKABLE void mark(const QString& category, const QString& event,
                          const QVariantMap& detail = QVariantMap());
    // Copy the live log to a timestamped snapshot file the owner can retrieve/share; returns its path.
    Q_INVOKABLE QString exportSnapshot();
    // The last `maxLines` formatted events (for an in-app view / copy-to-clipboard).
    Q_INVOKABLE QString recentText(int maxLines = 400) const;
    // Start a fresh timeline (e.g. right before reproducing an issue).
    Q_INVOKABLE void clearLog();

signals:
    void enabledChanged();
    void eventCountChanged();

private:
    void appendLocked(const QString& category, const QString& event, const QVariantMap& detail);
    void openFileLocked();

    static BaristaDiagnostics* s_instance;

    mutable QMutex m_mutex;
    bool m_enabled = true;
    quint64 m_seq = 0;
    QStringList m_ring;              // formatted lines, capped at m_ringCap
    int m_ringCap = 8000;
    QFile* m_file = nullptr;         // append-mode persistent log
    QString m_filePath;
    QString m_dir;
};
