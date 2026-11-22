find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

enable_language(RC)
set(CMAKE_AUTOMOC ON)

add_executable(bitstreamer WIN32
    main.cpp
    AppSettings.cpp
    AppSettings.h
    MainWindow.cpp
    MainWindow.h
    SettingsDialog.cpp
    SettingsDialog.h
    LevelMeter.h
    app.rc
)
target_link_libraries(bitstreamer PRIVATE bscore Qt6::Core Qt6::Gui Qt6::Widgets)
# Qt headers are not /W4 clean with every compiler version.
target_compile_options(bitstreamer PRIVATE /W3)

# Qt needs its platform and style plugins next to the executable.
set(_qt_plugin_dir "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}$<$<CONFIG:Debug>:/debug>/Qt6/plugins")
add_custom_command(TARGET bitstreamer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:bitstreamer>/platforms" "$<TARGET_FILE_DIR:bitstreamer>/styles"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_qt_plugin_dir}/platforms/qwindows$<$<CONFIG:Debug>:d>.dll" "$<TARGET_FILE_DIR:bitstreamer>/platforms/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_qt_plugin_dir}/styles/qmodernwindowsstyle$<$<CONFIG:Debug>:d>.dll" "$<TARGET_FILE_DIR:bitstreamer>/styles/"
    VERBATIM)

install(TARGETS bitstreamer RUNTIME DESTINATION .)
install(DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/platforms ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/styles DESTINATION .)
install(DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/
        DESTINATION .
        FILES_MATCHING PATTERN "Qt6*.dll" PATTERN "double-conversion*.dll" PATTERN "pcre2*.dll"
                       PATTERN "zlib*.dll" PATTERN "libz*.dll" PATTERN "md4c*.dll"
                       PATTERN "platforms" EXCLUDE PATTERN "styles" EXCLUDE)
