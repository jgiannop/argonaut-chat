cmake_minimum_required(VERSION 3.21)
project(argonaut-chat VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# CHAT_SERVER_ONLY: just argonaut-chat-server (a VPS: no widgets, no keyring,
# no tests), as deploy/setup-server.sh builds it.
option(CHAT_SERVER_ONLY "Build only argonaut-chat-server" OFF)
# The app's .deb: every feature required (the dock count needs Qt D-Bus), and
# the server left out of it (servers are set up by deploy/setup-server.sh).
option(CHAT_PACKAGE "Build the app's package (cpack)" OFF)

# 6.4 is what Ubuntu 24.04 ships, as for Argonaut and Argonaut Emails.
if(CHAT_SERVER_ONLY)
    find_package(Qt6 6.4 REQUIRED COMPONENTS Core Network Sql WebSockets)
else()
    # Multimedia: voice messages (the microphone, the speaker).
    find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets Network Sql WebSockets Multimedia)
    # The unread count on the dock's icon (the Unity launcher API).
    find_package(Qt6 6.4 QUIET COMPONENTS DBus)
    if(CHAT_PACKAGE AND NOT Qt6DBus_FOUND)
        message(FATAL_ERROR "The package is to have every feature, and Qt D-Bus is missing (the unread count on the dock)")
    endif()
endif()
# The cryptography: libsodium (1.0.18 on Ubuntu 24.04).
# The local database: SQLCipher (4.5 on Ubuntu 24.04, 4.13 on 26.04), and its key in the
# system keyring through QtKeychain.
if(WIN32)
    # On Windows both come from vcpkg (`vcpkg install libsodium sqlcipher`,
    # with its toolchain file), found by name: there is no pkg-config.
    find_path(SODIUM_INCLUDE_DIR sodium.h REQUIRED)
    find_library(SODIUM_LIBRARY NAMES libsodium sodium REQUIRED)
    add_library(chat_sodium INTERFACE)
    target_include_directories(chat_sodium INTERFACE ${SODIUM_INCLUDE_DIR})
    target_link_libraries(chat_sodium INTERFACE ${SODIUM_LIBRARY})
    find_path(SQLCIPHER_INCLUDE_DIR sqlite3.h PATH_SUFFIXES sqlcipher REQUIRED)
    find_library(SQLCIPHER_LIBRARY NAMES sqlcipher REQUIRED)
    add_library(chat_sqlcipher INTERFACE)
    target_include_directories(chat_sqlcipher INTERFACE ${SQLCIPHER_INCLUDE_DIR})
    target_link_libraries(chat_sqlcipher INTERFACE ${SQLCIPHER_LIBRARY})
    target_compile_definitions(chat_sqlcipher INTERFACE SQLITE_HAS_CODEC)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium>=1.0.18)
    # No version check: newer packages report the version of the SQLite inside
    # (3.51 for SQLCipher 4.13 on Ubuntu 26.04), older ones SQLCipher's own.
    pkg_check_modules(SQLCIPHER REQUIRED IMPORTED_TARGET sqlcipher)
    add_library(chat_sodium INTERFACE)
    target_link_libraries(chat_sodium INTERFACE PkgConfig::SODIUM)
    add_library(chat_sqlcipher INTERFACE)
    target_link_libraries(chat_sqlcipher INTERFACE PkgConfig::SQLCIPHER)
endif()
if(MSVC)
    # The sources are UTF-8 (✓, ⏱, …); no min/max macros from windows.h.
    add_compile_options(/utf-8 /permissive- /Zc:__cplusplus)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
if(NOT CHAT_SERVER_ONLY)
    find_package(Qt6Keychain REQUIRED)
endif()
qt_standard_project_setup()

include(GNUInstallDirs)

# The messenger's engine: no widgets, so all of it can be tested headless.
qt_add_library(chat_core STATIC
    src/core/Attachment.cpp src/core/Attachment.h
    src/core/ChatClient.cpp src/core/ChatClient.h
    src/core/ChatStore.cpp src/core/ChatStore.h
    src/core/CipherSql.cpp src/core/CipherSql.h
    src/core/Crypto.cpp src/core/Crypto.h
    src/core/KeyStore.cpp src/core/KeyStore.h
    src/core/Messenger.cpp src/core/Messenger.h
    src/core/Protocol.cpp src/core/Protocol.h
    src/core/RatchetSession.cpp src/core/RatchetSession.h
    src/core/SafetyNumber.cpp src/core/SafetyNumber.h
    src/core/Session.cpp src/core/Session.h
    src/core/Voice.cpp src/core/Voice.h
    src/core/X3dh.cpp src/core/X3dh.h
)
target_include_directories(chat_core PUBLIC src/core)
target_link_libraries(chat_core PUBLIC Qt6::Core Qt6::Network Qt6::Sql Qt6::WebSockets PRIVATE chat_sodium chat_sqlcipher)
target_compile_definitions(chat_core PUBLIC CHAT_VERSION="${PROJECT_VERSION}")

if(NOT CHAT_SERVER_ONLY)
# The window.
qt_add_library(chat_app STATIC
    src/app/DatabaseKey.cpp src/app/DatabaseKey.h
    src/app/Dialogs.cpp src/app/Dialogs.h
    src/app/MainWindow.cpp src/app/MainWindow.h
    src/app/Notifications.cpp src/app/Notifications.h
    src/app/SingleInstance.cpp src/app/SingleInstance.h
    src/app/StyleSheet.cpp src/app/StyleSheet.h
    src/app/Theme.cpp src/app/Theme.h
    src/app/TrayIcon.cpp src/app/TrayIcon.h
    src/app/VoiceAudio.cpp src/app/VoiceAudio.h
)
target_include_directories(chat_app PUBLIC src/app)
target_link_libraries(chat_app PUBLIC chat_core Qt6::Widgets Qt6::Multimedia PRIVATE Qt6Keychain::Qt6Keychain)
if(Qt6DBus_FOUND AND UNIX AND NOT APPLE)
    target_link_libraries(chat_app PRIVATE Qt6::DBus)
    target_compile_definitions(chat_app PRIVATE CHAT_DBUS)
endif()

qt_add_executable(argonaut-chat src/app/main.cpp)
target_link_libraries(argonaut-chat PRIVATE chat_app)
if(WIN32)
    # A window, not a console; and the program's icon, as Explorer and the taskbar show it.
    set_target_properties(argonaut-chat PROPERTIES WIN32_EXECUTABLE ON)
    target_sources(argonaut-chat PRIVATE data/argonaut-chat.rc)
endif()
install(TARGETS argonaut-chat RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(UNIX AND NOT APPLE)
    # The absolute path: the desktop doesn't use the user's PATH.
    set(CHAT_EXE "${CMAKE_INSTALL_FULL_BINDIR}/argonaut-chat")
    configure_file(data/argonaut-chat.desktop.in ${CMAKE_CURRENT_BINARY_DIR}/argonaut-chat.desktop @ONLY)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/argonaut-chat.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES data/argonaut-chat.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
    # Let menus and the dock see the new entry right away.
    install(CODE [[
        find_program(UPDATE_DESKTOP_DATABASE update-desktop-database)
        if(UPDATE_DESKTOP_DATABASE AND "$ENV{DESTDIR}" STREQUAL "")
            execute_process(COMMAND ${UPDATE_DESKTOP_DATABASE} "${CMAKE_INSTALL_PREFIX}/share/applications")
        endif()
    ]])
endif()
install(FILES LICENSE README.md DESTINATION ${CMAKE_INSTALL_DOCDIR})
endif()

# The relay server: a separate program, sharing chat_core (protocol, crypto).
qt_add_library(chat_server STATIC
    src/server/ChatServer.cpp src/server/ChatServer.h
    src/server/RateLimiter.cpp src/server/RateLimiter.h
    src/server/ServerStore.cpp src/server/ServerStore.h
)
target_include_directories(chat_server PUBLIC src/server)
target_link_libraries(chat_server PUBLIC chat_core)

qt_add_executable(argonaut-chat-server src/server/main.cpp)
target_link_libraries(argonaut-chat-server PRIVATE chat_server)
if(NOT CHAT_PACKAGE)
    install(TARGETS argonaut-chat-server RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# ------------------------------------------------------------- package ---
# The app as a .deb, as Argonaut Emails': `cmake --preset package && cmake
# --build build/package && cpack --config build/package/CPackConfig.cmake`, or
# packaging/build-debs.sh for each Ubuntu release in a clean container.
# Library dependencies are worked out by dpkg-shlibdeps; Qt's platform
# plugins are plugins, which it can't see, so they are named.
if(CHAT_PACKAGE)
    set(CPACK_PACKAGE_NAME "argonaut-chat")
    set(CPACK_PACKAGE_VENDOR "johngiannop")
    set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/jgiannop/argonaut-chat")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "A private messenger, encrypted end to end")
    set(CPACK_PACKAGE_DESCRIPTION "Argonaut Chat is a private messenger like Signal, written in C++ and Qt 6, \
the companion of the Argonaut file manager: messages encrypted end to end with the Signal Protocol's design \
(X3DH and the Double Ratchet, on libsodium), sign-up by username, safety numbers, message requests, and the \
history kept only on this computer, encrypted with SQLCipher, its key in the desktop's keyring.")
    set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
    set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}")
    set(CPACK_GENERATOR "DEB")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "johngiannop")
    set(CPACK_DEBIAN_PACKAGE_SECTION "net")
    set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/jgiannop/argonaut-chat")
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "qt6-qpa-plugins")
    set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "qt6-wayland, gnome-keyring | kwalletmanager")
    include(CPack)
endif()

if(NOT CHAT_SERVER_ONLY)
    include(CTest)
    if(BUILD_TESTING)
        add_subdirectory(tests)
    endif()
endif()
