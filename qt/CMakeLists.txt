cmake_minimum_required(VERSION 3.16)
project(nlink-ng VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Concurrent)

find_package(PkgConfig)
if(PkgConfig_FOUND)
  pkg_check_modules(LIBUSB libusb-1.0)
endif()
if(NOT LIBUSB_FOUND)
  find_path(LIBUSB_INCLUDE_DIR libusb.h PATH_SUFFIXES libusb-1.0)
  find_library(LIBUSB_LIBRARY NAMES usb-1.0 libusb-1.0)
  if(LIBUSB_INCLUDE_DIR AND LIBUSB_LIBRARY)
    set(LIBUSB_FOUND TRUE)
    set(LIBUSB_INCLUDE_DIRS "${LIBUSB_INCLUDE_DIR}")
    set(LIBUSB_LIBRARIES "${LIBUSB_LIBRARY}")
  endif()
endif()
if(NOT LIBUSB_FOUND)
  message(FATAL_ERROR "libusb-1.0 was not found")
endif()

set(NLINK_CARGO_TARGET "" CACHE STRING "Rust target triple for the Evo staticlib. Empty builds for the host.")
set(NLINK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../nlink")
if(NLINK_CARGO_TARGET)
  set(_nlink_rust_dir "${NLINK_ROOT}/target/${NLINK_CARGO_TARGET}/release")
else()
  set(_nlink_rust_dir "${NLINK_ROOT}/target/release")
endif()
if(MSVC)
  set(NLINK_LIB "${_nlink_rust_dir}/nlink.lib")
else()
  set(NLINK_LIB "${_nlink_rust_dir}/libnlink.a")
endif()
set(LIBNSPIRE "${NLINK_ROOT}/vendor/libnspire-sys/libnspire/src")

set(_nlink_cargo_env PKG_CONFIG_ALLOW_CROSS=1)
if(DEFINED ENV{PKG_CONFIG} AND NOT "$ENV{PKG_CONFIG}" STREQUAL "")
  list(APPEND _nlink_cargo_env "PKG_CONFIG=$ENV{PKG_CONFIG}")
endif()
if(DEFINED ENV{PKG_CONFIG_PATH} AND NOT "$ENV{PKG_CONFIG_PATH}" STREQUAL "")
  list(APPEND _nlink_cargo_env "PKG_CONFIG_PATH=$ENV{PKG_CONFIG_PATH}")
endif()
if(NLINK_CARGO_TARGET STREQUAL "x86_64-pc-windows-gnu")
  list(APPEND _nlink_cargo_env
    "CC_x86_64_pc_windows_gnu=x86_64-w64-mingw32-gcc"
    "CXX_x86_64_pc_windows_gnu=x86_64-w64-mingw32-g++"
    "CARGO_TARGET_X86_64_PC_WINDOWS_GNU_LINKER=x86_64-w64-mingw32-gcc"
  )
endif()
set(_nlink_cargo_target_arg)
if(NLINK_CARGO_TARGET)
  set(_nlink_cargo_target_arg --target "${NLINK_CARGO_TARGET}")
endif()

add_custom_command(
  OUTPUT "${NLINK_LIB}"
  COMMAND ${CMAKE_COMMAND} -E env ${_nlink_cargo_env}
          cargo build --offline --locked --release
          --manifest-path "${NLINK_ROOT}/Cargo.toml"
          ${_nlink_cargo_target_arg}
  WORKING_DIRECTORY "${NLINK_ROOT}"
  COMMENT "Building nlink Evo Rust static library"
)
add_custom_target(nlink_rust DEPENDS "${NLINK_LIB}")

add_executable(n-link
  src/main.cpp
  src/MainWindow.cpp
  src/MainWindow.h
  src/PathNavigator.cpp
  src/PathNavigator.h
  icons.qrc
  "${NLINK_ROOT}/cpp/archive.cpp"
  "${NLINK_ROOT}/cpp/cli.cpp"
  "${NLINK_ROOT}/cpp/dbus.cpp"
  "${NLINK_ROOT}/cpp/device.cpp"
  "${NLINK_ROOT}/cpp/dusb.cpp"
  "${NLINK_ROOT}/cpp/ffi.cpp"
  "${NLINK_ROOT}/cpp/json.cpp"
  "${NLINK_ROOT}/cpp/nspire_err.cpp"
  "${NLINK_ROOT}/cpp/paths.cpp"
  "${NLINK_ROOT}/cpp/png.cpp"
  "${NLINK_ROOT}/cpp/progress.cpp"
  "${NLINK_ROOT}/cpp/romdump.cpp"
  "${NLINK_ROOT}/cpp/ti8x.cpp"
  "${NLINK_ROOT}/cpp/viewframe.cpp"
  "${LIBNSPIRE}/cx2.cpp"
  "${LIBNSPIRE}/data.c"
  "${LIBNSPIRE}/error.c"
  "${LIBNSPIRE}/init.c"
  "${LIBNSPIRE}/packet.c"
  "${LIBNSPIRE}/service.c"
  "${LIBNSPIRE}/usb.c"
  "${LIBNSPIRE}/services/devinfo.c"
  "${LIBNSPIRE}/services/dir.c"
  "${LIBNSPIRE}/services/file.c"
  "${LIBNSPIRE}/services/os.c"
  "${LIBNSPIRE}/services/screenshot.c"
  "${LIBNSPIRE}/services/view.c"
)
add_dependencies(n-link nlink_rust)
if(WIN32)
  set_target_properties(n-link PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

target_include_directories(n-link PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${NLINK_ROOT}/cpp"
  "${LIBNSPIRE}"
  "${LIBNSPIRE}/services"
  "${LIBNSPIRE}/api"
  ${LIBUSB_INCLUDE_DIRS}
)
if(LIBUSB_LIBRARY_DIRS)
  target_link_directories(n-link PRIVATE ${LIBUSB_LIBRARY_DIRS})
endif()

if(MSVC)
  find_package(ZLIB REQUIRED)
  target_link_libraries(n-link PRIVATE
    Qt6::Widgets
    Qt6::Concurrent
    "${NLINK_LIB}"
    ZLIB::ZLIB
    ${LIBUSB_LIBRARIES}
    ws2_32
    userenv
    bcrypt
    ntdll
    ole32
    shell32
    advapi32
  )
  target_link_options(n-link PRIVATE "/WHOLEARCHIVE:${NLINK_LIB}")
else()
  target_link_libraries(n-link PRIVATE
    Qt6::Widgets
    Qt6::Concurrent
    "-Wl,--whole-archive"
    "${NLINK_LIB}"
    "-Wl,--no-whole-archive"
    z
    ${LIBUSB_LIBRARIES}
  )
  if(WIN32)
    target_link_libraries(n-link PRIVATE ws2_32 userenv bcrypt ntdll ole32 shell32 advapi32 pthread)
  else()
    target_link_libraries(n-link PRIVATE pthread dl m)
  endif()
endif()

install(TARGETS n-link RUNTIME DESTINATION bin)
if(UNIX)
  install(FILES nlink-ng.desktop DESTINATION share/applications)
  install(FILES icons/icon.png DESTINATION share/icons/hicolor/512x512/apps RENAME nlink-ng.png)
endif()
