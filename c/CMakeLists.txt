cmake_minimum_required(VERSION 3.25)
project(brightness C)

set(CMAKE_WIN32_EXECUTABLE ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(${PROJECT_NAME}
    main.c
    resource.rc
    resource.h
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    UNICODE
    _UNICODE
    _WIN32_WINNT=0x0A00
)

target_compile_options(${PROJECT_NAME} PRIVATE
    /W4 /permissive- /utf-8 /Zi
    $<$<CONFIG:Release,MinSizeRel>:/O2 /GL /Gy /GS->
    $<$<CONFIG:Debug>:/Od /RTC1>
)

target_link_options(${PROJECT_NAME} PRIVATE
    $<$<CONFIG:Release,MinSizeRel>:/LTCG /OPT:REF /OPT:ICF /INCREMENTAL:NO>
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    user32
    shell32
    comctl32
    dxva2
    setupapi
)
