# SPDX-FileCopyrightText: None
#
# SPDX-License-Identifier: CC0-1.0

set(BIN_NAME deepin-update-manager)

set(DUM_SOURCES
    main.cpp
    ManagerAdaptor.h
    ManagerAdaptor.cpp
    Branch.h
    Branch.cpp
    Idle.cpp
    Idle.h
)

qt_add_dbus_interface(DUM_SOURCES org.freedesktop.systemd1.Manager.xml SystemdManagerInterface)
qt_add_dbus_interface(DUM_SOURCES org.freedesktop.systemd1.Unit.xml SystemdUnitInterface)

add_executable(${BIN_NAME}
    ${DUM_SOURCES}
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
