#include "baristadiagnostics.h"

#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QStandardPaths>
#include <QTextStream>
#include <QSettings>

BaristaDiagnostics* BaristaDiagnostics::s_instance = nullptr;

namespace {
// Render a detail map as compact "key=value key=value", stable order, no newlines.
QString renderDetail(const QVariantMap& detail)
{
    if (detail.isEmpty())
        return QString();
    QStringList parts;
    parts.reserve(detail.size());
    for (auto it = detail.constBegin(); it != detail.constEnd(); ++it) {
        QString v = it.value().toString();
        v.replace('\n', QLatin1String("\\n"));
        if (v.size() > 200)
            v = v.left(200) + QStringLiteral("…");
        parts << (it.key() + '=' + v);
    }
    return parts.join(' ');
}
}  // namespace

BaristaDiagnostics::BaristaDiagnostics(QObject* parent)
    : QObject(parent)
{
    QSettings settings;
    m_enabled = settings.value(QStringLiteral("barista/diagnosticsEnabled"), true).toBool();

    // Prefer the user-visible Documents dir so the log is retrievable without a file manager
    // deep-dive; fall back to app data if Documents is unavailable.
    QString base = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (base.isEmpty())
        base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    m_dir = base + QStringLiteral("/DecenzaBaristaDiagnostics");
    QDir().mkpath(m_dir);
    m_filePath = m_dir + QStringLiteral("/barista-diagnostics.log");

    s_instance = this;

    // A boot marker anchors every session and proves the recorder is live.
    record(QStringLiteral("system"), QStringLiteral("recorder_started"),
           {{QStringLiteral("enabled"), m_enabled}, {QStringLiteral("path"), m_filePath}});
}

BaristaDiagnostics::~BaristaDiagnostics()
{
    QMutexLocker lock(&m_mutex);
    if (s_instance == this)
        s_instance = nullptr;
    if (m_file) {
        m_file->close();
        delete m_file;
        m_file = nullptr;
    }
}

void BaristaDiagnostics::record(const QString& category, const QString& event,
                                const QVariantMap& detail)
{
    BaristaDiagnostics* self = s_instance;
    if (!self)
        return;
    QMutexLocker lock(&self->m_mutex);
    if (!self->m_enabled)
        return;
    self->appendLocked(category, event, detail);
}

void BaristaDiagnostics::mark(const QString& category, const QString& event,
                              const QVariantMap& detail)
{
    {
        QMutexLocker lock(&m_mutex);
        if (!m_enabled)
            return;
        appendLocked(category, event, detail);
    }
    emit eventCountChanged();
}

// Caller holds m_mutex.
void BaristaDiagnostics::appendLocked(const QString& category, const QString& event,
                                      const QVariantMap& detail)
{
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz"));
    const QString detailStr = renderDetail(detail);
    QString line = QStringLiteral("%1  #%2  [%3] %4")
                       .arg(stamp)
                       .arg(m_seq++, 5, 10, QLatin1Char('0'))
                       .arg(category, -9)
                       .arg(event);
    if (!detailStr.isEmpty())
        line += QStringLiteral("  ") + detailStr;

    m_ring.append(line);
    if (m_ring.size() > m_ringCap)
        m_ring.removeFirst();

    if (!m_file)
        openFileLocked();
    if (m_file && m_file->isOpen()) {
        QTextStream ts(m_file);
        ts << line << '\n';
        ts.flush();   // crash-safe: the timeline on disk is always current to the last event
    }
}

// Caller holds m_mutex.
void BaristaDiagnostics::openFileLocked()
{
    QDir().mkpath(m_dir);
    // Rotate a large existing log so the file the owner exports stays a manageable size.
    QFileInfo fi(m_filePath);
    if (fi.exists() && fi.size() > 4 * 1024 * 1024) {
        QFile::remove(m_filePath + QStringLiteral(".prev"));
        QFile::rename(m_filePath, m_filePath + QStringLiteral(".prev"));
    }
    m_file = new QFile(m_filePath);
    if (!m_file->open(QIODevice::Append | QIODevice::Text)) {
        delete m_file;
        m_file = nullptr;
    }
}

bool BaristaDiagnostics::enabled() const
{
    QMutexLocker lock(&m_mutex);
    return m_enabled;
}

void BaristaDiagnostics::setEnabled(bool on)
{
    {
        QMutexLocker lock(&m_mutex);
        if (m_enabled == on)
            return;
        m_enabled = on;
    }
    QSettings().setValue(QStringLiteral("barista/diagnosticsEnabled"), on);
    record(QStringLiteral("system"), on ? QStringLiteral("logging_enabled")
                                        : QStringLiteral("logging_disabled"));
    emit enabledChanged();
}

int BaristaDiagnostics::eventCount() const
{
    QMutexLocker lock(&m_mutex);
    return static_cast<int>(m_seq);
}

QString BaristaDiagnostics::recentText(int maxLines) const
{
    QMutexLocker lock(&m_mutex);
    if (maxLines <= 0 || m_ring.size() <= maxLines)
        return m_ring.join('\n');
    return QStringList(m_ring.mid(m_ring.size() - maxLines)).join('\n');
}

QString BaristaDiagnostics::exportSnapshot()
{
    QMutexLocker lock(&m_mutex);
    const QString stampName = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    const QString outPath = m_dir + QStringLiteral("/barista-diagnostics-") + stampName + QStringLiteral(".log");
    QFile out(outPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Text))
        return QString();
    QTextStream ts(&out);
    ts << QStringLiteral("# Decenza barista diagnostics snapshot ")
       << QDateTime::currentDateTime().toString(Qt::ISODate) << '\n'
       << QStringLiteral("# ") << m_ring.size() << QStringLiteral(" events\n\n");
    ts << m_ring.join('\n') << '\n';
    ts.flush();
    out.close();
    return outPath;
}

void BaristaDiagnostics::clearLog()
{
    {
        QMutexLocker lock(&m_mutex);
        m_ring.clear();
        m_seq = 0;
        if (m_file) {
            m_file->close();
            delete m_file;
            m_file = nullptr;
        }
        QFile::remove(m_filePath);
    }
    record(QStringLiteral("system"), QStringLiteral("log_cleared"));
    emit eventCountChanged();
}
