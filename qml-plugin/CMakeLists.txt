cmake_minimum_required(VERSION 3.16)
project(owe-lockfeed VERSION 0.1.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick)

add_library(owe_lockfeed MODULE
  src/lockfeed.cpp
  src/plugin.cpp
  src/lockfeed.h
)

target_link_libraries(owe_lockfeed PRIVATE Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick)

install(TARGETS owe_lockfeed
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/qt6/qml/Owe/LockFeed
)
install(FILES src/qmldir
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/qt6/qml/Owe/LockFeed
)
