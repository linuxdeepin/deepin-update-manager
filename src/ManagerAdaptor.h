#pragma once

#include "SystemdManagerInterface.h"
#include "SystemdUnitInterface.h"

#include <QLocalServer>
#include <QObject>

struct Progress {
    QString stage;
    float percent;
};

class ManagerAdaptor : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.deepin.UpdateManager1")
    Q_PROPERTY(bool upgradable READ upgradable NOTIFY upgradableChanged)
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)

public:
    ManagerAdaptor(int upgradeStdoutFd, const QDBusConnection &bus, QObject *parent = nullptr);
    ~ManagerAdaptor() override;

    /* dbus start */
public slots:
    Q_SCRIPTABLE void checkUpgrade(const QDBusMessage &message);
    Q_SCRIPTABLE void upgrade(const QDBusMessage &message);

public slots:
    bool upgradable();
    QString state();

signals:
    void upgradableChanged(bool upgradable);
    void stateChanged(const QString &state);

signals:
    Q_SCRIPTABLE void progress(const Progress &progress);
    /* dbus end */

private:
    QDBusConnection m_bus;
    QLocalServer *m_server;
    org::freedesktop::systemd1::Manager *m_systemdManager;
    org::freedesktop::systemd1::Unit *m_dumUpgradeUnit;

    bool m_upgradable;
    QString m_state;

private slots:
    void onDumUpgradeUnitPropertiesChanged(const QString &interfaceName,
                                           const QVariantMap &changedProperties,
                                           const QStringList &invalidatedProperties);

private:
    void parseUpgradeStdoutLine(const QByteArray &line);
};
