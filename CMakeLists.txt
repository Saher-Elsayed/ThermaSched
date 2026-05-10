cmake_minimum_required(VERSION 3.20)
project(thermasched C)

set(CMAKE_C_STANDARD 11)

# ARM bare-metal toolchain (set via -DCMAKE_TOOLCHAIN_FILE)
# Example: cmake .. -DCMAKE_TOOLCHAIN_FILE=../cmake/arm-none-eabi.cmake

# Compiler flags for ARM Cortex-A53 bare-metal
set(CMAKE_C_FLAGS
    "-mcpu=cortex-a53 -mfpu=neon-vfpv4 -mfloat-abi=hard "
    "-O2 -ffunction-sections -fdata-sections "
    "-Wall -Wextra -Wno-unused-parameter "
    "-DARM_BAREMETAL "
    "-fno-exceptions -fno-rtti"
)
set(CMAKE_EXE_LINKER_FLAGS "-Wl,--gc-sections -Wl,-Map=thermasched.map")

# Source files
set(SOURCES
    src/scheduler/thermasched.c
    src/thermal/thermal_model.c
    src/noc/noc_model.c
    src/dvfs/dvfs_ctrl.c
    src/buffer/weight_buffer.c
    src/thermal/sysmon.c
)

# Include directories
include_directories(
    src/scheduler
    src/thermal
    src/noc
    src/dvfs
    src/buffer
)

# Main firmware executable
add_executable(thermasched.elf ${SOURCES} src/main.c)

# Link with math library
target_link_libraries(thermasched.elf m)

# Post-build: generate binary and hex files for JTAG programming
add_custom_command(TARGET thermasched.elf POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary thermasched.elf thermasched.bin
    COMMAND ${CMAKE_OBJCOPY} -O ihex   thermasched.elf thermasched.hex
    COMMENT "Generating binary and hex files"
)

# Install targets
install(FILES
    ${CMAKE_BINARY_DIR}/thermasched.elf
    ${CMAKE_BINARY_DIR}/thermasched.bin
    DESTINATION firmware
)
