// SPDX-FileCopyrightText: 2025 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef UPGRADETASK_H
#define UPGRADETASK_H

#include "abstracttask.h"

#include <QLocalServer>

class UpgradeTask : public AbstractTask
{
    Q_OBJECT
public:
    explicit UpgradeTask(const QDBusConnection &bus, const QDBusMessage &message, QObject *parent = nullptr);
    ~UpgradeTask() override;

    void setTaskDate(const QString &unitName, int fd);
    bool run() override;

signals:
    void progressChanged(const QString &stage, float percent);
    void upgradableChanged(bool upgradable);
    void stateChanged(const QString &state);

public slots:
    void onDumUpgradeUnitPropertiesChanged(const QString &interfaceName,
                                           const QVariantMap &changedProperties,
                                           const QStringList &invalidatedProperties);    

private:
    void parseUpgradeStdoutLine(const QByteArray &line);

private:
    QLocalServer *m_upgradeStdoutServer;
    QDBusMessage m_message;
    QString m_unitName;
    int m_fd;

    QString m_state;
};

#endif // UPGRADETASK_H
