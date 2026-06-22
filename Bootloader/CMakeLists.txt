cmake_minimum_required(VERSION 3.22)

# Bootloader CMakeLists.txt
# Build independently: cmake -B build_bl -S Bootloader

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release")
endif()

set(CMAKE_PROJECT_NAME STM32L431_BL)
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

enable_language(C ASM)

# ============================================================================
# Toolchain settings (inline, avoid depending on parent toolchain file)
# ============================================================================
set(TARGET_FLAGS "-mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=hard")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${TARGET_FLAGS} -Wall -fdata-sections -ffunction-sections")
set(CMAKE_ASM_FLAGS "${CMAKE_C_FLAGS} -x assembler-with-cpp -MMD -MP")
set(CMAKE_C_FLAGS_DEBUG "-Og -g3")
set(CMAKE_C_FLAGS_RELEASE "-Os -g0")

# Linker script for bootloader (16KB region)
set(LINKER_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/STM32L431_BL.ld")
set(CMAKE_EXE_LINKER_FLAGS "${TARGET_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -T \"${LINKER_SCRIPT}\"")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} --specs=nano.specs --specs=nosys.specs")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-Map=${CMAKE_PROJECT_NAME}.map -Wl,--gc-sections")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--print-memory-usage")

# ============================================================================
# Sources
# ============================================================================
set(BL_SOURCES
    Src/bl_main.c
    Src/bl_core.c
    Src/bl_meta.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Components/OTA/flash_port.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Components/Util/util_uart.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../startup_stm32l431xx.s
    ${CMAKE_CURRENT_SOURCE_DIR}/../Core/Src/system_stm32l4xx.c
)

# Minimal HAL sources needed by bootloader
set(HAL_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../Drivers/STM32L4xx_HAL_Driver/Src/stm32l4xx_hal.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Drivers/STM32L4xx_HAL_Driver/Src/stm32l4xx_hal_cortex.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Drivers/STM32L4xx_HAL_Driver/Src/stm32l4xx_hal_rcc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Drivers/STM32L4xx_HAL_Driver/Src/stm32l4xx_hal_rcc_ex.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Drivers/STM32L4xx_HAL_Driver/Src/stm32l4xx_hal_flash.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Drivers/STM32L4xx_HAL_Driver/Src/stm32l4xx_hal_flash_ex.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Drivers/STM32L4xx_HAL_Driver/Src/stm32l4xx_hal_gpio.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Drivers/STM32L4xx_HAL_Driver/Src/stm32l4xx_hal_pwr.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Drivers/STM32L4xx_HAL_Driver/Src/stm32l4xx_hal_pwr_ex.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Drivers/STM32L4xx_HAL_Driver/Src/stm32l4xx_hal_uart.c
)

# ============================================================================
# Target
# ============================================================================
add_executable(${CMAKE_PROJECT_NAME} ${BL_SOURCES} ${HAL_SOURCES})

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Components/OTA
    ${CMAKE_CURRENT_SOURCE_DIR}/../Components/Util
    ${CMAKE_CURRENT_SOURCE_DIR}/../Core/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Drivers/STM32L4xx_HAL_Driver/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Drivers/STM32L4xx_HAL_Driver/Inc/Legacy
    ${CMAKE_CURRENT_SOURCE_DIR}/../Drivers/CMSIS/Device/ST/STM32L4xx/Include
    ${CMAKE_CURRENT_SOURCE_DIR}/../Drivers/CMSIS/Include
)

target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    USE_HAL_DRIVER
    STM32L431xx
)

# ============================================================================
# Post-build: generate .bin and .hex
# ============================================================================
set(OBJCOPY arm-none-eabi-objcopy)
set(SIZE arm-none-eabi-size)

add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
    COMMAND ${SIZE} $<TARGET_FILE:${CMAKE_PROJECT_NAME}>
    COMMAND ${OBJCOPY} -O ihex $<TARGET_FILE:${CMAKE_PROJECT_NAME}> ${CMAKE_PROJECT_NAME}.hex
    COMMAND ${OBJCOPY} -O binary $<TARGET_FILE:${CMAKE_PROJECT_NAME}> ${CMAKE_PROJECT_NAME}.bin
    COMMENT "Generating ${CMAKE_PROJECT_NAME}.hex and .bin"
)
