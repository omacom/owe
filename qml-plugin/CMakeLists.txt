cmake_minimum_required(VERSION 3.16)
project(owe-lockfeed VERSION 0.2.7 LANGUAGES CXX)
include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick)

add_library(owe_lockfeed MODULE
  src/lockfeed.cpp
  src/plugin.cpp
  src/lockfeed.h
)

target_link_libraries(owe_lockfeed PRIVATE Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick)

include(CTest)
if(BUILD_TESTING)
  find_package(Qt6 REQUIRED COMPONENTS Test)
  add_executable(owe_lockfeed_test test/lockfeed.cpp src/lockfeed.cpp src/lockfeed.h)
  target_include_directories(owe_lockfeed_test PRIVATE src)
  target_link_libraries(owe_lockfeed_test PRIVATE Qt6::Quick Qt6::Test)
  add_test(NAME lockfeed COMMAND owe_lockfeed_test)
  set_tests_properties(lockfeed PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QPA_PLATFORMTHEME=;QT_STYLE_OVERRIDE="
    TIMEOUT 30
  )
endif()

install(TARGETS owe_lockfeed
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/qt6/qml/Owe/LockFeed
)
install(FILES src/qmldir
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/qt6/qml/Owe/LockFeed
)
