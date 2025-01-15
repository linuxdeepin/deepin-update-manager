//
// Created by uos on 25-1-16.
//

#ifndef INSTALLER_H
#define INSTALLER_H
#include "Branch.h"

#include <qspan.h>

// Installer: 安装器
class Installer
{
public:
    Installer();
    ~Installer();
    void doInstall();
private:
    Branch *m_branch;
};
#endif //INSTALLER_H
