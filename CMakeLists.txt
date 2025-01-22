# SPDX-FileCopyrightText: None
#
# SPDX-License-Identifier: CC0-1.0

cmake_minimum_required(VERSION 3.16.0)
project(deepin-update-manager
    VERSION 0.1.0
    LANGUAGES C CXX
)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

include(GNUInstallDirs)

find_package(Qt6 REQUIRED COMPONENTS Core DBus Network)
find_package(PolkitQt6-1 REQUIRED)
find_package(PkgConfig)

pkg_check_modules(libsystemd REQUIRED IMPORTED_TARGET libsystemd)
pkg_check_modules(openssl REQUIRED IMPORTED_TARGET openssl)

pkg_search_module(systemd REQUIRED systemd)
pkg_get_variable(SYSUSERS_DIR systemd sysusers_dir)
pkg_get_variable(SYSTEMD_SYSTEM_UNIT_DIR systemd systemd_system_unit_dir)

pkg_search_module(DBUS REQUIRED dbus-1)
pkg_get_variable(DBUS_DATA_DIR dbus-1 datadir)
if(DBUS_DATA_DIR)
    set(DBUS_DATA_DIR "${DBUS_DATA_DIR}/dbus-1")
else()
    set(DBUS_DATA_DIR "${CMAKE_INSTALL_DATADIR}/dbus-1")
endif()

add_subdirectory(src)

install(
    FILES
        misc/polkit-1/actions/org.deepin.UpdateManager.policy
    DESTINATION
        ${CMAKE_INSTALL_DATADIR}/polkit-1/actions
)

install(
    FILES
        misc/polkit-1/rules.d/org.deepin.update-manager.rules
    DESTINATION
        ${CMAKE_INSTALL_DATADIR}/polkit-1/rules.d
)

install(
    FILES misc/sysusers.d/deepin-update-manager.conf
    DESTINATION ${SYSUSERS_DIR}
)

install(
    FILES
        misc/systemd/system/deepin-update-manager.service
        misc/systemd/system/dum-list-remote-refs-stdout.socket
        misc/systemd/system/dum-list-remote-refs.service
        misc/systemd/system/dum-upgrade-stdout.socket
        misc/systemd/system/dum-upgrade@.service
    DESTINATION
        ${SYSTEMD_SYSTEM_UNIT_DIR}
)

install(
    FILES misc/dbus/system.d/org.deepin.UpdateManager1.conf
    DESTINATION ${DBUS_DATA_DIR}/system.d
)

install(
    FILES misc/dbus/system-services/org.deepin.UpdateManager1.service
    DESTINATION ${DBUS_DATA_DIR}/system-services
)
