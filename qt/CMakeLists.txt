cmake_minimum_required(VERSION 3.16)
project(nlink-ng VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Concurrent)

set(NLINK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../nlink")
set(NLINK_LIB "${NLINK_ROOT}/target/release/libnlink.a")

add_custom_command(
  OUTPUT "${NLINK_LIB}"
  COMMAND cargo build --release --manifest-path "${NLINK_ROOT}/Cargo.toml"
  WORKING_DIRECTORY "${NLINK_ROOT}"
  COMMENT "Building nlink Rust static library"
)
add_custom_target(nlink_rust DEPENDS "${NLINK_LIB}")

add_executable(n-link
  src/main.cpp
  src/MainWindow.cpp
  src/MainWindow.h
  icons.qrc
)
add_dependencies(n-link nlink_rust)

target_include_directories(n-link PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(n-link PRIVATE
  Qt6::Widgets
  Qt6::Concurrent
  "${NLINK_LIB}"
  usb-1.0
  pthread
  dl
  m
)

install(TARGETS n-link RUNTIME DESTINATION bin)
install(FILES nlink-ng.desktop DESTINATION share/applications)
install(FILES icons/icon.png DESTINATION share/icons/hicolor/512x512/apps RENAME nlink-ng.png)
