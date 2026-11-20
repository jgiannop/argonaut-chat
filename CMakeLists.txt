cmake_minimum_required(VERSION 3.21)
project(argonaut-chat VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 6.4 is what Ubuntu 24.04 ships, as for Argonaut and Argonaut Emails.
find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets Network Sql WebSockets)
# The cryptography: libsodium (1.0.18 on Ubuntu 24.04).
find_package(PkgConfig REQUIRED)
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium>=1.0.18)
qt_standard_project_setup()

include(GNUInstallDirs)

# The messenger's engine: no widgets, so all of it can be tested headless.
qt_add_library(chat_core STATIC
    src/core/ChatClient.cpp src/core/ChatClient.h
    src/core/ChatStore.cpp src/core/ChatStore.h
    src/core/Crypto.cpp src/core/Crypto.h
    src/core/Protocol.cpp src/core/Protocol.h
    src/core/RatchetSession.cpp src/core/RatchetSession.h
    src/core/SafetyNumber.cpp src/core/SafetyNumber.h
    src/core/Session.cpp src/core/Session.h
    src/core/X3dh.cpp src/core/X3dh.h
)
target_include_directories(chat_core PUBLIC src/core)
target_link_libraries(chat_core PUBLIC Qt6::Core Qt6::Network Qt6::Sql Qt6::WebSockets PRIVATE PkgConfig::SODIUM)
target_compile_definitions(chat_core PUBLIC CHAT_VERSION="${PROJECT_VERSION}")

# The window.
qt_add_library(chat_app STATIC
    src/app/MainWindow.cpp src/app/MainWindow.h
    src/app/StyleSheet.cpp src/app/StyleSheet.h
    src/app/Theme.cpp src/app/Theme.h
)
target_include_directories(chat_app PUBLIC src/app)
target_link_libraries(chat_app PUBLIC chat_core Qt6::Widgets)

qt_add_executable(argonaut-chat src/app/main.cpp)
target_link_libraries(argonaut-chat PRIVATE chat_app)
install(TARGETS argonaut-chat RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# The relay server: a separate program, sharing chat_core (protocol, crypto).
qt_add_library(chat_server STATIC
    src/server/ChatServer.cpp src/server/ChatServer.h
    src/server/ServerStore.cpp src/server/ServerStore.h
)
target_include_directories(chat_server PUBLIC src/server)
target_link_libraries(chat_server PUBLIC chat_core)

qt_add_executable(argonaut-chat-server src/server/main.cpp)
target_link_libraries(argonaut-chat-server PRIVATE chat_server)
install(TARGETS argonaut-chat-server RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
