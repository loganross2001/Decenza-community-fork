#pragma once

#include <QObject>
#include <QSettings>

// [barista-fork] The barista module's OWN settings, stored under its own "barista/" QSettings
// group. Deliberately NOT a Settings-facade domain sub-object: keeping it self-contained means
// the upstream Settings classes are never edited (the whole point of the modular fork). QML
// reaches it as `Barista.settings.<prop>`.
class AssistantSettings : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)

public:
    explicit AssistantSettings(QObject* parent = nullptr);

    bool enabled() const;
    void setEnabled(bool on);

signals:
    void enabledChanged();

private:
    mutable QSettings m_settings;  // org/app default = DecentEspresso/DE1Qt (set in main)
};
