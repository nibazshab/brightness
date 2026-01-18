cmake_minimum_required(VERSION 3.20)
project(brightness C)

set(CMAKE_WIN32_EXECUTABLE ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(${PROJECT_NAME}
    main.c
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    UNICODE
    _UNICODE
    _WIN32_WINNT=0x0A00
)

target_compile_options(${PROJECT_NAME} PRIVATE
    /MT
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    user32
    gdi32
    shell32
    comctl32
    dxva2
)
