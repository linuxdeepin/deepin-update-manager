// SPDX-FileCopyrightText: 2024 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "SystemdManagerInterface.h"
#include "SystemdUnitInterface.h"
#include "checker.h"
#include "downloader.h"
#include "idle.h"
#include "installer.h"

#include <QLocalServer>
#include <QObject>

#define DBUS_PATH "/org/deepin/UpdateManager1"
#define DBUS_SERVICE_NAME "org.deepin.UpdateManager1"
#define DBUS_INTERFACE_NAME DBUS_SERVICE_NAME

struct Progress
{
    QString stage;
    float percent;
};
Q_DECLARE_METATYPE(Progress)

QDBusArgument &operator<<(QDBusArgument &argument, const Progress &progress);
const QDBusArgument &operator>>(const QDBusArgument &argument, Progress &progress);

class UpdateManager : public QObject, public QDBusContext
{
    Q_OBJECT
    Q_PROPERTY(bool checkUpdateMode READ checkUpdateMode WRITE setCheckUpdateMode)
    Q_PROPERTY(bool upgradeMode READ upgradeMode WRITE setUpgradeMode)
    Q_PROPERTY(bool upgradable READ upgradable)
    Q_PROPERTY(QString state READ state)
    Q_PROPERTY(QString currentBranch READ currentBranch)
    Q_PROPERTY(QStringList allBranches READ allBranches)

public:
    UpdateManager(int listRemoteRefsFd, int upgradeStdoutFd,QObject *parent = nullptr);
    ~UpdateManager() override;

private:
    void setPropertyUpgradable(bool upgradable);
    void setPropertyState(const QString &state);
    void setPropertyCurrentBranch(const QString &branch);
    void setPropertyAllBranches(const QStringList &branches);
    static void setCheckUpdateMode(uint16_t checkUpdateMode);
    static void setUpgradeMode(uint16_t upgradeMode);

    /* dbus start */
public slots:
    Q_SCRIPTABLE void checkUpgrade();
    Q_SCRIPTABLE void upgrade();

public slots:
    bool upgradable() const;
    QString state() const;
    QString currentBranch() const;
    QStringList allBranches() const;
    uint16_t checkUpdateMode() const;
    uint16_t upgradeMode() const;
    void inhibitIdle(const QString& task) {m_idle.Inhibit(task);}
    void unInhibitIdle(const QString& task) {m_idle.UnInhibit(task);}

Q_SIGNALS:
    Q_SCRIPTABLE void progress(const Progress &progress);
    /* dbus end */

private:
    QLocalServer *m_listRemoteRefsStdoutServer;
    QLocalServer *m_upgradeStdoutServer;
    org::freedesktop::systemd1::Manager *m_systemdManager;
    org::freedesktop::systemd1::Unit *m_dumUpgradeUnit;
    QString m_remoteBranch;

    // properties
    bool m_upgradable;
    QString m_state;
    QStringList m_allBranches;
    QString m_currentBranch;
    Idle m_idle;
    uint16_t m_checkUpdateMode;
    uint16_t m_upgradeMode;
    Checker *m_checker;
    Downloader  *m_downloader;
    Installer *m_installer;


private slots:
    void onDumUpgradeUnitPropertiesChanged(const QString &interfaceName,
                                           const QVariantMap &changedProperties,
                                           const QStringList &invalidatedProperties);

private:
    void parseUpgradeStdoutLine(const QByteArray &line);
    void sendPropertyChanged(const QString &property, const QVariant &value);
    bool checkAuthorization(const QString &actionId, const QString &service) const;
};
