cmake_minimum_required(VERSION 3.20 FATAL_ERROR)

project(LCD LANGUAGES C)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

find_package(SDL2 CONFIG REQUIRED)

set(LCD_Srcs
"src/lcd.h"
"src/main.c"
)

add_executable(LCD WIN32 ${LCD_Srcs})
target_include_directories(LCD PRIVATE "src")
target_link_libraries(LCD SDL2 SDL2main m) # Note for windows port: don't need to link libm when using msvc
