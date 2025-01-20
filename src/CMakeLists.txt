set(BIN_NAME deepin-update-manager)

set(DUM_SOURCES
        main.cpp
        updatemanager.h
        updatemanager.cpp
        Branch.h
        Branch.cpp
        idle.cpp
        idle.h
        checker.cpp
        checker.h
        downloader.cpp
        downloader.h
        installer.cpp
        installer.h
        updatePlatform.cpp
        updatePlatform.h
)

qt_add_dbus_adaptor( DUM_ADAPTER_SOURCES
        ../dbus/org.deepin.UpdateManager1.xml
        updatemanager.h
        UpdateManager
        dumdbusadaptor
        DumDbusAdaptor
)

qt_add_dbus_interface(DUM_SOURCES ../dbus/org.freedesktop.systemd1.Manager.xml SystemdManagerInterface)
qt_add_dbus_interface(DUM_SOURCES ../dbus/org.freedesktop.systemd1.Unit.xml SystemdUnitInterface)

add_executable(${BIN_NAME}
    ${DUM_SOURCES}
    ${DUM_ADAPTER_SOURCES}
)

target_link_libraries(${BIN_NAME} PRIVATE
    PkgConfig::libsystemd
    PkgConfig::openssl
    Qt6::Core
    Qt6::DBus
    Qt6::Network
    PolkitQt6-1::Core
)

install(
    TARGETS ${BIN_NAME}
    RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}
)
