// SPDX-FileCopyrightText: 2024 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ManagerAdaptor.h"

#include "common.h"
#include "checkupgradetask.h"
#include "upgradetask.h"

QDBusArgument &operator<<(QDBusArgument &argument, const Progress &progress)
{
    argument.beginStructure();
    argument << progress.stage << progress.percent;
    argument.endStructure();

    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, Progress &progress)
{
    argument.beginStructure();
    argument >> progress.stage;
    double d;
    argument >> d;
    progress.percent = d;
    argument.endStructure();

    return argument;
}

ManagerAdaptor::ManagerAdaptor(int listRemoteRefsFd,
                               int upgradeStdoutFd,
                               const QDBusConnection &bus,
                               QObject *parent)
    : QObject(parent)
    , m_bus(bus)
    , m_listRemoteRefsFd(listRemoteRefsFd)
    , m_upgradeStdoutFd(upgradeStdoutFd)
    , m_remoteBranch(QString())
    , m_upgradable(false)
    , m_state(STATE_IDEL)
    , m_idle(new Idle(this))
    , m_settings(new QSettings(DUM_STATE_FILE, QSettings::IniFormat, this))
{
    qRegisterMetaType<Progress>("Progress");
    qDBusRegisterMetaType<Progress>();

    // 如果文件存在，则说明是空闲退出且没有重启过。读取settings配置恢复之前的状态
    if (QFile::exists(DUM_STATE_FILE)) {
        loadStatusFromFile();
    }

    connect(this, &ManagerAdaptor::stateChanged, this, [this](const QString &state) {
        sendPropertyChanged("state", state);
    });
    connect(this, &ManagerAdaptor::upgradableChanged, this, [this](bool upgradable) {
        sendPropertyChanged("upgradable", upgradable);
    });
}

ManagerAdaptor::~ManagerAdaptor() = default;

void ManagerAdaptor::checkUpgrade(const QDBusMessage &message)
{
    m_idle->Inhibit("checkUpgrade");
    checkUpgradeTask *task = new checkUpgradeTask(m_bus, message, this);
    task->setTaskDate("dum-list-remote-refs.service", m_listRemoteRefsFd);

    connect(task, &checkUpgradeTask::checkUpgradeResult, this, [this](bool upgradable, const QString &remoteBranch) {
        m_remoteBranch = remoteBranch;
        if (m_upgradable != upgradable) {
            m_upgradable = upgradable;
            emit upgradableChanged(m_upgradable);
        }
        saveStatusToFile();
   });

   task->run();
   m_idle->UnInhibit("checkUpgrade");
}

static QString systemdEscape(const QString &str)
{
    auto tmp = str;
    tmp.replace('-', "\\x2d");
    tmp.replace('/', "-");

    return tmp;
}

void ManagerAdaptor::upgrade(const QDBusMessage &message)
{
    if (!m_upgradable || m_remoteBranch.isEmpty()) {
        m_bus.send(message.createErrorReply(QDBusError::AccessDenied, "No upgrade available"));
        return;
    }

    m_idle->Inhibit("upgrade");
    QString version = OSTREE_DEFAULT_REMOTE_NAME + ':' + m_remoteBranch;
    QString unit = QString("dum-upgrade@%1.service").arg(systemdEscape(version));

    UpgradeTask *task = new UpgradeTask(m_bus, message, this);
    task->setTaskDate("dum-upgrade.service", m_upgradeStdoutFd);

    connect(task, &UpgradeTask::progressChanged, this, [this](const QString &stage, float percent) {
        emit progress({ stage, percent });
    });

    connect(task, &UpgradeTask::upgradableChanged, this, [this](bool upgradable) {
        m_upgradable = upgradable;
        emit upgradableChanged(m_upgradable);
        saveStatusToFile();
        m_idle->UnInhibit("upgrade");
    });

    connect(task, &UpgradeTask::stateChanged, this, [this](const QString &state) {
        m_state = state;
        emit stateChanged(m_state);
        saveStatusToFile();
    });

    if (!task->run()) {
        m_idle->UnInhibit("upgrade");
    }
}

bool ManagerAdaptor::upgradable() const
{
    return m_upgradable;
}

QString ManagerAdaptor::state() const
{
    return m_state;
}

void ManagerAdaptor::sendPropertyChanged(const QString &property, const QVariant &value)
{
    auto msg = QDBusMessage::createSignal(ADAPTOR_PATH,
                                          "org.freedesktop.DBus.Properties",
                                          "PropertiesChanged");
    msg << "org.deepin.UpdateManager1";
    msg << QVariantMap{ { property, value } };
    msg << QStringList{};

    auto res = m_bus.send(msg);
    if (!res) {
        qWarning() << "sendPropertyChanged failed";
    }
}

void ManagerAdaptor::loadStatusFromFile()
{
    if (!m_settings)
        return;

    m_state = m_settings->value("state",STATE_IDEL).toString();
    m_upgradable = m_settings->value("upgradable",false).toBool();
    m_remoteBranch = m_settings->value("remoteBranch","").toString();
}

void ManagerAdaptor::saveStatusToFile()
{
    if (!m_settings)
        return;

    m_settings->setValue("state", m_state);
    m_settings->setValue("upgradable", m_upgradable);
    m_settings->setValue("remoteBranch", m_remoteBranch);
    m_settings->sync();
}
