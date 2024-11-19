#include "ManagerAdaptor.h"

#include <systemd/sd-daemon.h>

#include <QCoreApplication>
#include <QDBusConnection>

#include <string>
#include <unordered_map>

const std::string DUM_UPGRADE_STDOUT = "dum-upgrade-stdout";

static std::unordered_map<std::string, int> getFds()
{
    std::unordered_map<std::string, int> fds;
    char **names;
    int count = sd_listen_fds_with_names(true, &names);
    for (int i = 0; i < count; i++) {
        fds.emplace(names[i], SD_LISTEN_FDS_START + i);
        free(names[i]);
    }

    return fds;
}

int main(int argc, char *argv[])
{
    auto fds = getFds();
    if (!fds.contains(DUM_UPGRADE_STDOUT)) {
        return 1;
    }

    int dumUpgradeStdoutFd = fds[DUM_UPGRADE_STDOUT];

    QCoreApplication a(argc, argv);

    QDBusConnection connection = QDBusConnection::systemBus();

    ManagerAdaptor adaptor(dumUpgradeStdoutFd, connection);
    connection.registerService("org.deepin.UpdateManager1");
    connection.registerObject("/org/deepin/UpdateManager1",
                              &adaptor,
                              QDBusConnection::ExportScriptableContents);

    return a.exec();
}
