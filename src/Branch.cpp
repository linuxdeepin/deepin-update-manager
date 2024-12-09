// SPDX-FileCopyrightText: 2024 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Branch.h"

#include <QList>
#include <QVersionNumber>

Branch::Branch(const QString &str)
{
    auto tmp = str.split('/');
    if (tmp.size() < 5) {
        // throw std::invalid_argument("Branch format error");
        return;
    }

    m_codeName = tmp[0];
    m_period = tmp[1];
    m_version = tmp[2];
    m_arch = tmp[3];

    if (tmp[4] == COMPONENT_BASE || tmp[4] == COMPONENT_SECURITY) {
        m_component = tmp[4];

        if (tmp.size() > 5) {
            m_revision = tmp[5];
        }
    } else { // 商业项目
        m_project = tmp[4];
        m_component = tmp[5];

        if (tmp.size() > 6) {
            m_revision = tmp[6];
        }
    }
}

QDBusArgument Branch::operator<<(QDBusArgument &argument) const
{
    argument.beginStructure();
    argument << m_codeName;
    argument << m_period;
    argument << m_version;
    argument << m_arch;
    argument << m_project;
    argument << m_component;
    argument << m_revision;
    argument.endStructure();

    return argument;
}

const QDBusArgument Branch::operator>>(const QDBusArgument &argument)
{
    argument.beginStructure();
    argument >> m_codeName;
    argument >> m_period;
    argument >> m_version;
    argument >> m_arch;
    argument >> m_project;
    argument >> m_component;
    argument >> m_revision;
    argument.endStructure();

    return argument;
}

bool Branch::valid() const
{
    if (m_codeName.isEmpty()) {
        return false;
    }

    // period 只能是 develop 或 release
    if (m_period != PERIOD_DEVELOP && m_period != PERIOD_RELEASE) {
        return false;
    }

    // component 只能是 base 或 security
    if (m_component != COMPONENT_BASE && m_component != COMPONENT_SECURITY) {
        return false;
    }


    // 非商业项目 base 不能有 revision
    if (m_project.isEmpty() && m_component == COMPONENT_BASE && !m_revision.isEmpty()) {
        return false;
    }

    // security 不能没有 revision
    if (m_component == COMPONENT_SECURITY && m_revision.isEmpty()) {
        return false;
    }

    return true;
}

bool Branch::canUpgradeTo(const Branch &dest) const
{
    if (dest.valid()) {
        return false;
    }

    if (dest.m_project != m_project) {
        // 非同商业项目
        return false;
    }

    auto version = QVersionNumber::fromString(m_version);
    auto destVersion = QVersionNumber::fromString(dest.m_version);
    if (destVersion > version) {
        return true;
    }

    if (dest.m_revision > m_revision) {
        return true;
    }

    return false;
}

QString Branch::toString() const
{
    QString str;
    str += m_codeName + "/" + m_period + "/" + m_version + "/" + m_arch;

    if (!m_project.isEmpty()) {
        str += "/" + m_project;
    }

    str += "/" + m_component;

    if (!m_revision.isEmpty()) {
        str += "/" + m_revision;
    }

    return str;
}
