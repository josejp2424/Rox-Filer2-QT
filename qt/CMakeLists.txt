cmake_minimum_required(VERSION 3.22)
project(RoxFiler2QT VERSION 1.0.7 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)
include(GNUInstallDirs)

# A direct CMake invocation uses the distribution Qt6 by default.
# build.sh can explicitly enable Essora's /opt SDK or pass a custom prefix.
option(ROX_QT_PREFER_ESSORA "Prefer /opt/essora/qt6 if no Qt SDK is selected" OFF)
if(ROX_QT_PREFER_ESSORA AND NOT DEFINED Qt6_DIR AND NOT DEFINED CMAKE_PREFIX_PATH AND
   (EXISTS "/opt/essora/qt6/lib/cmake/Qt6/Qt6Config.cmake" OR
    EXISTS "/opt/essora/qt6/lib64/cmake/Qt6/Qt6Config.cmake"))
    list(PREPEND CMAKE_PREFIX_PATH "/opt/essora/qt6")
endif()
find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets DBus)
find_package(Qt6 6.4 QUIET COMPONENTS Svg)
message(STATUS "Rox-Filer2-QT: Qt ${Qt6_VERSION} from ${Qt6_DIR}")

# lrelease is necessary for all 11 translated UIs; fail clearly if unavailable.
find_package(Qt6 QUIET COMPONENTS LinguistTools)
if(TARGET Qt6::lrelease)
    set(ROX_QT_LRELEASE "$<TARGET_FILE:Qt6::lrelease>")
else()
    find_program(ROX_QT_LRELEASE NAMES lrelease lrelease-qt6 lrelease6
      HINTS "${QT_HOST_PATH}/bin" "${Qt6_DIR}/../../../bin" /usr/lib/qt6/bin
      REQUIRED)
endif()

set(ROX_QT_LANGUAGES ar ca de es fr it pt hu ja ru zh)
set(ROX_QT_QM_FILES)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/translations")
foreach(lang IN LISTS ROX_QT_LANGUAGES)
    set(ts "${CMAKE_CURRENT_SOURCE_DIR}/translations/rox-filer2-qt_${lang}.ts")
    set(qm "${CMAKE_CURRENT_BINARY_DIR}/translations/rox-filer2-qt_${lang}.qm")
    add_custom_command(OUTPUT "${qm}"
        COMMAND "${ROX_QT_LRELEASE}" "${ts}" -qm "${qm}"
        DEPENDS "${ts}" VERBATIM)
    list(APPEND ROX_QT_QM_FILES "${qm}")
endforeach()
add_custom_target(rox-qt-translations ALL DEPENDS ${ROX_QT_QM_FILES})

add_executable(rox-qt-mount-helper src/rox_qt_mount_helper.c)
add_executable(rox-qt-root-launcher src/rox_qt_root_launcher.c)
add_executable(rox-qt-smb-helper src/rox_qt_smb_helper.c)
foreach(helper IN ITEMS rox-qt-mount-helper rox-qt-root-launcher rox-qt-smb-helper)
    target_compile_options(${helper} PRIVATE -Wall -Wextra -Wpedantic)
endforeach()

add_executable(rox-filer2-qt
    src/main.cpp
    src/themebridge.h src/rootpalette.h
    src/mainwindow.cpp src/mainwindow.h
    src/drivemount.cpp src/drivemount.h
    src/imagevolumes.cpp src/imagevolumes.h
    src/imagemounter.cpp src/imagemounter.h
    src/browserpage.cpp src/browserpage.h
    src/filepane.cpp src/filepane.h
    src/pathbar.cpp src/pathbar.h
    src/mimeapps.cpp src/mimeapps.h
    src/iconprovider.cpp src/iconprovider.h
    src/thumbnailer.cpp src/thumbnailer.h
    src/filemodel.cpp src/filemodel.h
    src/hoverpreview.cpp src/hoverpreview.h
    src/filedrop.cpp src/filedrop.h
    src/fileops.cpp src/fileops.h
    src/transferprogress.cpp src/transferprogress.h
    src/filedialogs.cpp src/filedialogs.h
    src/roxopen.cpp src/roxopen.h
    src/roxoptions.cpp src/roxoptions.h
    src/findexpr.cpp src/findexpr.h
    src/xattrs.cpp src/xattrs.h
    data/rox-qt.qrc
)
target_compile_options(rox-filer2-qt PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(rox-filer2-qt PRIVATE
    ROX_QT_VERSION="1.0.7"
    ROX_QT_TRANSLATION_DIR="/usr/share/rox-filer2-qt/translations")
target_link_libraries(rox-filer2-qt PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::DBus)
add_dependencies(rox-filer2-qt rox-qt-translations)
if(TARGET Qt6::Svg)
    target_link_libraries(rox-filer2-qt PRIVATE Qt6::Svg)
    target_compile_definitions(rox-filer2-qt PRIVATE ROX_QT_HAVE_QTSVG=1)
endif()

add_executable(rox-qt-drives src/qt_drives.cpp src/drivemount.cpp src/drivemount.h
    src/imagevolumes.cpp src/imagevolumes.h
    src/imagemounter.cpp src/imagemounter.h)
target_compile_definitions(rox-qt-drives PRIVATE ROX_QT_VERSION="1.0.7")
target_link_libraries(rox-qt-drives PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::DBus)
target_compile_options(rox-qt-drives PRIVATE -Wall -Wextra -Wpedantic)

# When built with /opt/essora/qt6 the Qt runtime must be the same one.
if(Qt6_DIR MATCHES "^/opt/essora/qt6/")
    set_target_properties(rox-filer2-qt rox-qt-drives PROPERTIES
       BUILD_RPATH "/opt/essora/qt6/lib;/opt/essora/qt6/lib64;/opt/essora/qt6/lib/x86_64-linux-gnu"
       INSTALL_RPATH "/opt/essora/qt6/lib;/opt/essora/qt6/lib64;/opt/essora/qt6/lib/x86_64-linux-gnu")
endif()

# Qt-only binary + resources; deliberately do not overwrite ROX GTK3.
install(TARGETS rox-filer2-qt rox-qt-drives RUNTIME DESTINATION bin)
install(FILES data/rox-filer2-qt.desktop data/roxfiler-qt-root.desktop
    DESTINATION share/applications)
install(FILES data/rox-filer2-qt.svg data/rox-filer2-qt-root.svg
    DESTINATION share/icons/hicolor/scalable/apps)
install(FILES data/rox-filer2-qt.png data/rox-filer2-qt-root.png
    DESTINATION share/icons/hicolor/256x256/apps)
install(FILES data/rox-qt-drives.desktop DESTINATION /etc/xdg/autostart)
install(FILES ${ROX_QT_QM_FILES} DESTINATION share/rox-filer2-qt/translations)
install(DIRECTORY resources/help/ DESTINATION share/rox-filer2-qt/help)
install(TARGETS rox-qt-mount-helper rox-qt-root-launcher rox-qt-smb-helper
    RUNTIME DESTINATION /usr/lib/rox-filer2-qt)
install(PROGRAMS data/roxfiler-qt-root data/qt-polkit-session
    DESTINATION /usr/bin)
install(DIRECTORY data/polkit/ DESTINATION /usr/share/polkit-1/actions FILES_MATCHING PATTERN "*.policy")
install(PROGRAMS data/rox-qt DESTINATION bin)
option(ROX_QT_INSTALL_ROX_COMMAND "Install /usr/bin/rox (conflicts with GTK3 ROX)" OFF)
if(ROX_QT_INSTALL_ROX_COMMAND)
    install(PROGRAMS data/rox-qt DESTINATION bin RENAME rox)
endif()

option(ROX_QT_BUILD_TESTS "Build and run Qt file-operation regression tests" OFF)
if(ROX_QT_BUILD_TESTS)
    enable_testing()
    add_executable(rox-qt-fileops-test
        tests/test_fileops.cpp
        src/fileops.cpp src/fileops.h
        src/transferprogress.cpp src/transferprogress.h
        src/imagevolumes.cpp src/imagevolumes.h
        src/imagemounter.cpp src/imagemounter.h
        src/mimeapps.cpp src/mimeapps.h
    )
    target_include_directories(rox-qt-fileops-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(rox-qt-fileops-test PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets)
    add_test(NAME sonic_file_operations COMMAND rox-qt-fileops-test)
    set_tests_properties(sonic_file_operations PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        TIMEOUT 45)
    add_executable(rox-qt-transferprogress-test
        tests/test_transferprogress.cpp
        src/fileops.cpp src/fileops.h
        src/transferprogress.cpp src/transferprogress.h
        src/imagevolumes.cpp src/imagevolumes.h
        src/imagemounter.cpp src/imagemounter.h
        src/mimeapps.cpp src/mimeapps.h
    )
    target_include_directories(rox-qt-transferprogress-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(rox-qt-transferprogress-test PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets)
    add_test(NAME transfer_progress COMMAND rox-qt-transferprogress-test)
    set_tests_properties(transfer_progress PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        TIMEOUT 90)
    add_executable(rox-qt-dnd-test
        tests/test_dragdrop.cpp
        src/filedrop.cpp src/filedrop.h
        src/fileops.cpp src/fileops.h
        src/transferprogress.cpp src/transferprogress.h
        src/imagevolumes.cpp src/imagevolumes.h
        src/imagemounter.cpp src/imagemounter.h
        src/mimeapps.cpp src/mimeapps.h
    )
    target_include_directories(rox-qt-dnd-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(rox-qt-dnd-test PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets)
    add_test(NAME sonic_dragdrop COMMAND rox-qt-dnd-test)
    set_tests_properties(sonic_dragdrop PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        TIMEOUT 45)
    add_executable(rox-qt-open-test
        tests/test_roxopen.cpp
        src/roxopen.cpp src/roxopen.h
        src/roxoptions.cpp src/roxoptions.h
        src/mimeapps.cpp src/mimeapps.h
    )
    target_include_directories(rox-qt-open-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(rox-qt-open-test PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets)
    add_test(NAME sonic_open_semantics COMMAND rox-qt-open-test)
    set_tests_properties(sonic_open_semantics PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        TIMEOUT 45)
    add_executable(rox-qt-find-test
        tests/test_findexpr.cpp
        src/findexpr.cpp src/findexpr.h
    )
    target_include_directories(rox-qt-find-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(rox-qt-find-test PRIVATE Qt6::Core)
    add_test(NAME sonic_select_if COMMAND rox-qt-find-test)
    set_tests_properties(sonic_select_if PROPERTIES TIMEOUT 45)
    add_executable(rox-qt-xattr-test
        tests/test_xattrs.cpp
        src/xattrs.cpp src/xattrs.h
    )
    target_include_directories(rox-qt-xattr-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(rox-qt-xattr-test PRIVATE Qt6::Core)
    add_test(NAME sonic_xattrs COMMAND rox-qt-xattr-test)
    set_tests_properties(sonic_xattrs PROPERTIES TIMEOUT 45)
    add_executable(rox-qt-clicks-test
        tests/test_clicks.cpp
        src/filepane.cpp src/filepane.h
        src/filemodel.cpp src/filemodel.h
        src/iconprovider.cpp src/iconprovider.h
        src/thumbnailer.cpp src/thumbnailer.h
        src/hoverpreview.cpp src/hoverpreview.h
        src/filedrop.cpp src/filedrop.h
        src/fileops.cpp src/fileops.h
        src/transferprogress.cpp src/transferprogress.h
        src/imagevolumes.cpp src/imagevolumes.h
        src/imagemounter.cpp src/imagemounter.h
        src/filedialogs.cpp src/filedialogs.h
        src/mimeapps.cpp src/mimeapps.h
        src/roxopen.cpp src/roxopen.h
        src/roxoptions.cpp src/roxoptions.h
        src/findexpr.cpp src/findexpr.h
        src/xattrs.cpp src/xattrs.h
        data/rox-qt.qrc
    )
    target_include_directories(rox-qt-clicks-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_definitions(rox-qt-clicks-test PRIVATE ROX_QT_VERSION="test")
    target_link_libraries(rox-qt-clicks-test PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::DBus)
    if(TARGET Qt6::Svg)
        target_link_libraries(rox-qt-clicks-test PRIVATE Qt6::Svg)
        target_compile_definitions(rox-qt-clicks-test PRIVATE ROX_QT_HAVE_QTSVG=1)
    endif()
    add_test(NAME sonic_click_semantics COMMAND rox-qt-clicks-test)
    set_tests_properties(sonic_click_semantics PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        TIMEOUT 120)
    add_executable(rox-qt-imagevolumes-test
        tests/test_imagevolumes.cpp src/imagevolumes.cpp src/imagevolumes.h)
    target_include_directories(rox-qt-imagevolumes-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(rox-qt-imagevolumes-test PRIVATE Qt6::Core)
    add_test(NAME image_volume_discovery COMMAND rox-qt-imagevolumes-test)
    add_executable(rox-qt-imagemounter-test
        tests/test_imagemounter.cpp
        src/imagemounter.cpp src/imagemounter.h
        src/imagevolumes.cpp src/imagevolumes.h
        src/drivemount.cpp src/drivemount.h)
    target_include_directories(rox-qt-imagemounter-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(rox-qt-imagemounter-test PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::DBus)
    add_test(NAME image_mounter COMMAND rox-qt-imagemounter-test)
    set_tests_properties(image_mounter PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 180)
    set_tests_properties(image_volume_discovery PROPERTIES TIMEOUT 15)
    add_executable(rox-qt-window-test
        tests/test_mainwindow.cpp
        src/mainwindow.cpp src/mainwindow.h
        src/drivemount.cpp src/drivemount.h
        src/imagevolumes.cpp src/imagevolumes.h
        src/imagemounter.cpp src/imagemounter.h
        src/browserpage.cpp src/browserpage.h
        src/pathbar.cpp src/pathbar.h
        src/filepane.cpp src/filepane.h
        src/filemodel.cpp src/filemodel.h
        src/iconprovider.cpp src/iconprovider.h
        src/thumbnailer.cpp src/thumbnailer.h
        src/hoverpreview.cpp src/hoverpreview.h
        src/filedrop.cpp src/filedrop.h
        src/fileops.cpp src/fileops.h
        src/transferprogress.cpp src/transferprogress.h
        src/filedialogs.cpp src/filedialogs.h
        src/mimeapps.cpp src/mimeapps.h
        src/roxopen.cpp src/roxopen.h
        src/roxoptions.cpp src/roxoptions.h
        src/findexpr.cpp src/findexpr.h
        src/xattrs.cpp src/xattrs.h
        data/rox-qt.qrc
    )
    target_include_directories(rox-qt-window-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_definitions(rox-qt-window-test PRIVATE ROX_QT_VERSION="test")
    target_link_libraries(rox-qt-window-test PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::DBus)
    if(TARGET Qt6::Svg)
        target_link_libraries(rox-qt-window-test PRIVATE Qt6::Svg)
        target_compile_definitions(rox-qt-window-test PRIVATE ROX_QT_HAVE_QTSVG=1)
    endif()
    add_test(NAME sonic_window_smoke COMMAND rox-qt-window-test)
    set_tests_properties(sonic_window_smoke PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        TIMEOUT 120)
endif()
