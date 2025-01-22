// SPDX-FileCopyrightText: 2024 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "SystemdManagerInterface.h"
#include "SystemdUnitInterface.h"
#include "Idle.h"

#include <QLocalServer>
#include <QObject>
#include <QSettings>

#define ADAPTOR_PATH "/org/deepin/UpdateManager1"

struct Progress
{
    QString stage;
    float percent;
};
Q_DECLARE_METATYPE(Progress)

QDBusArgument &operator<<(QDBusArgument &argument, const Progress &progress);
const QDBusArgument &operator>>(const QDBusArgument &argument, Progress &progress);

class ManagerAdaptor : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.deepin.UpdateManager1")
    Q_PROPERTY(bool upgradable READ upgradable NOTIFY upgradableChanged SCRIPTABLE true)
    Q_PROPERTY(QString state READ state NOTIFY stateChanged SCRIPTABLE true)

public:
    ManagerAdaptor(int listRemoteRefsFd,
                   int upgradeStdoutFd,
                   const QDBusConnection &bus,
                   QObject *parent = nullptr);
    ~ManagerAdaptor() override;

    /* dbus start */
public slots:
    Q_SCRIPTABLE void checkUpgrade(const QDBusMessage &message);
    Q_SCRIPTABLE void upgrade(const QDBusMessage &message);

public slots:
    bool upgradable() const;
    QString state() const;

signals:
    void upgradableChanged(bool upgradable);
    void stateChanged(const QString &state);

signals:
    Q_SCRIPTABLE void progress(const Progress &progress);
    /* dbus end */

private:
    QDBusConnection m_bus;
    QLocalServer *m_listRemoteRefsStdoutServer;
    QLocalServer *m_upgradeStdoutServer;
    org::freedesktop::systemd1::Manager *m_systemdManager;
    org::freedesktop::systemd1::Unit *m_dumUpgradeUnit;
    QString m_remoteBranch;

    bool m_upgradable;
    QString m_state;
    Idle *m_idle;
    QSettings *m_settings;

private slots:
    void onDumUpgradeUnitPropertiesChanged(const QString &interfaceName,
                                           const QVariantMap &changedProperties,
                                           const QStringList &invalidatedProperties);

private:
    void parseUpgradeStdoutLine(const QByteArray &line);
    void sendPropertyChanged(const QString &property, const QVariant &value);
    bool checkAuthorization(const QString &actionId, const QString &service) const;
    void loadStatus();
};
