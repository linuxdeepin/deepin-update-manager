// SPDX-FileCopyrightText: 2025 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CHECKUPGRADETASK_H
#define CHECKUPGRADETASK_H

#include "abstracttask.h"

#include <QLocalServer>

class checkUpgradeTask : public AbstractTask
{
    Q_OBJECT
public:
    explicit checkUpgradeTask(const QDBusConnection &bus, const QDBusMessage &message, QObject *parent = nullptr);
    ~checkUpgradeTask() override;

    void setTaskDate(const QString &unitName, int fd);
    bool run() override;

signals:
    void checkUpgradeResult(bool upgradable, const QString &remoteBranch);

private:
    QLocalServer *m_listRemoteRefsStdoutServer;
    QDBusMessage m_message;
    QString m_unitName;
    int m_fd;
    
};

#endif // CHECKUPGRADETASK_H
