cmake_minimum_required(VERSION 3.20)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_CXX_FLAGS "-Wall -Wextra -pedantic")
set(CMAKE_C_FLAGS "-Wall -Wextra -pedantic")

set(ROCM_PATH
    ""
    CACHE PATH "Path to ROCm install directory")

if("${ROCM_PATH}" STREQUAL "")
  message(
    FATAL_ERROR
      "\n ROCM_PATH not set"
      "Please provide it using: cmake -DROCM_PATH=/path/to/rocm/install \n")
endif()

set(CMAKE_C_COMPILER "${ROCM_PATH}/bin/amdclang")
set(CMAKE_CXX_COMPILER "${ROCM_PATH}/bin/amdclang++")

project(amd-gpu-tools LANGUAGES CXX)
# ALL REGULAR TOOLS

add_executable(update-note update-note.cpp)
target_include_directories(
  update-note SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third-party/msgpack-c/include)

add_executable(update-note-phdr update-note-phdr.cpp)
target_include_directories(
  update-note-phdr SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third-party/ELFIO)

add_executable(extract-fatbin extract-fatbin.cpp)
target_include_directories(
  extract-fatbin SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third-party/ELFIO)

add_executable(extract-gpubin extract-gpubin.cpp)

add_executable(update-fatbin update-fatbin.cpp)

add_executable(update-exec update-exec.cpp)
target_include_directories(
  update-exec SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third-party/ELFIO)

# SPECIAL CASE FOR PRELOAD

# Paths for hipcc and the preload file
set(HIPCC "${ROCM_PATH}/bin/hipcc")
set(PRELOAD_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/preload.cpp")
set(PRELOAD_SO "${CMAKE_CURRENT_BINARY_DIR}/preload.so")

# Actual command to build preload.so
add_custom_command(
  OUTPUT "${PRELOAD_SO}"
  COMMAND ${HIPCC} -D__HIP_PLATFORM_AMD__ -x c++ -shared -fpic
          -I"${ROCM_PATH}"/include/ "${PRELOAD_SOURCE}" -o "${PRELOAD_SO}"
  DEPENDS "${PRELOAD_SOURCE}"
  COMMENT "Building ${PRELOAD_SOURCE} with ${HIPCC}"
  VERBATIM)

add_custom_target(PreloadFile ALL DEPENDS "${PRELOAD_SO}")

# SPECIAL CASE FOR instr-driver

# Paths for instr-driver
set(DRIVER_SCRIPT_SRC "${CMAKE_CURRENT_SOURCE_DIR}/instr-driver")
set(DRIVER_SCRIPT_DEST "${CMAKE_CURRENT_BINARY_DIR}/instr-driver")

# Command to copy instr-driver
add_custom_command(
  OUTPUT "${DRIVER_SCRIPT_DEST}"
  COMMAND ${CMAKE_COMMAND} -E copy "${DRIVER_SCRIPT_SRC}"
          "${DRIVER_SCRIPT_DEST}"
  DEPENDS "${DRIVER_SCRIPT_SRC}"
  COMMENT "Copying ${DRIVER_SCRIPT_SRC} to build directory"
  VERBATIM)

add_custom_target(DriverScript ALL DEPENDS "${DRIVER_SCRIPT_DEST}")
