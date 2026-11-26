cmake_minimum_required(VERSION 3.20)
project(stm32_serial_console LANGUAGES C)

if(NOT WIN32)
    message(FATAL_ERROR "This project uses the Win32 serial API.")
endif()
enable_language(RC)

add_subdirectory(third_party/CUI EXCLUDE_FROM_ALL)
add_executable(stm32_serial_console src/main.c src/serial_port.c src/favorites.c src/text_decode.c
               src/serial_ansi.c src/ssh_session.c src/log_archive.c src/serial_control.c)
if(MINGW)
    # windres preprocessing cannot reliably pass non-ASCII absolute paths on Windows.
    set(app_icon_object "${CMAKE_CURRENT_BINARY_DIR}/app_icon.obj")
    add_custom_command(OUTPUT "${app_icon_object}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/src/app.rc" app_icon.rc
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/src/app.ico" app.ico
        COMMAND "${CMAKE_RC_COMPILER}" -O coff -i app_icon.rc -o app_icon.obj
        DEPENDS src/app.rc src/app.ico
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/app_icon.rc" "${CMAKE_CURRENT_BINARY_DIR}/app.ico"
        VERBATIM)
    set_source_files_properties("${app_icon_object}" PROPERTIES EXTERNAL_OBJECT TRUE)
    target_sources(stm32_serial_console PRIVATE "${app_icon_object}")
else()
    target_sources(stm32_serial_console PRIVATE src/app.rc)
    set_property(SOURCE src/app.rc APPEND PROPERTY OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/app.ico")
    set_property(SOURCE src/app.rc APPEND PROPERTY INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/src")
endif()
target_compile_features(stm32_serial_console PRIVATE c_std_11)
target_link_libraries(stm32_serial_console PRIVATE cui::cui comdlg32 advapi32)
cui_app_target(stm32_serial_console)
if(MSVC)
    target_compile_options(stm32_serial_console PRIVATE "$<$<COMPILE_LANGUAGE:C>:/utf-8;/W4>")
    target_compile_options(cui PRIVATE /utf-8)
else()
    # CUI's designated-initializer macros intentionally zero a nested options member.
    target_compile_options(stm32_serial_console PRIVATE "$<$<COMPILE_LANGUAGE:C>:-Wall;-Wextra;-Wno-missing-braces>")
endif()
