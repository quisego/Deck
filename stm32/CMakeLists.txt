cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)


# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME stm32)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# Shared protocol library (root/common/proto)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../common/proto ${CMAKE_BINARY_DIR}/proto)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

file(GLOB U8G2_SOURCES "ThirdParty/u8g2/*.c")

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user sources here
    Common/Logger/logger.c
    Common/Supervisor/supervisor.c
    Common/FSM/fsm.c
    Common/Link/link.c
    Common/Memstat/mem_stat.c

    BSP/Button/button.c
    BSP/ssd1306/ssd1306.c
    BSP/bme280/bme280.c
    BSP/ds1307/ds1307.c
    BSP/I2C_BUS/i2c_bus.c

    ${U8G2_SOURCES}

    Cmd/App/app.c
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined include paths
    Common
    Common/Logger
    Common/FSM
    Common/Supervisor
    Common/Link
    Common/Memstat

    BSP/Button
    BSP/ssd1306
    BSP/bme280
    BSP/ds1307
    BSP/I2C_BUS

    ThirdParty/u8g2

    Cmd/App
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
    proto
)
