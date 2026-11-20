cmake_minimum_required(VERSION 3.21)
project(argonaut-chat VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 6.4 is what Ubuntu 24.04 ships, as for Argonaut and Argonaut Emails.
find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets Network Sql)
qt_standard_project_setup()

include(GNUInstallDirs)

# The messenger's engine: no widgets, so all of it can be tested headless.
qt_add_library(chat_core STATIC
    src/core/ChatStore.cpp src/core/ChatStore.h
)
target_include_directories(chat_core PUBLIC src/core)
target_link_libraries(chat_core PUBLIC Qt6::Core Qt6::Network Qt6::Sql)
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

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
