cmake_minimum_required(VERSION 3.20)

if(APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS deployment target")
endif()

project(MikuMikuWorld VERSION 0.5.2 LANGUAGES C CXX)

if(ANDROID)
    add_compile_options(-O2)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(MIKUMIKUWORLD_IOS TRUE)
    enable_language(OBJCXX)
endif()
set(MIKUMIKUWORLD_VERSION_STRING "0.5.2")
option(MIKUMIKUWORLD_SEKAI_LIVE_RENDERING "Build the separate SEKAI live note renderer" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

file(GLOB_RECURSE MIKUMIKUWORLD_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/MikuMikuWorld/*.cpp"
)

list(APPEND MIKUMIKUWORLD_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Depends/glad/src/glad.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/Depends/stb_vorbis/stb_vorbis.c"
)
list(FILTER MIKUMIKUWORLD_SOURCES EXCLUDE REGEX "/Android(Main|ImGuiBackend)\\.cpp$")

if(ANDROID)
    list(FILTER MIKUMIKUWORLD_SOURCES EXCLUDE REGEX "/main\\.cpp$")
    list(FILTER MIKUMIKUWORLD_SOURCES EXCLUDE REGEX "/imgui_impl_glfw\\.cpp$")
    list(APPEND MIKUMIKUWORLD_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/MikuMikuWorld/AndroidMain.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/MikuMikuWorld/AndroidImGuiBackend.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/MikuMikuWorld/AndroidFileDialog.cpp"
        "${CMAKE_ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c"
    )
    add_library(main SHARED ${MIKUMIKUWORLD_SOURCES})
    set(MIKUMIKUWORLD_TARGET main)
elseif(MIKUMIKUWORLD_IOS)
    list(FILTER MIKUMIKUWORLD_SOURCES EXCLUDE REGEX "/main\\.cpp$")
    list(FILTER MIKUMIKUWORLD_SOURCES EXCLUDE REGEX "/imgui_impl_glfw\\.cpp$")
    add_executable(MikuMikuWorld MACOSX_BUNDLE ${MIKUMIKUWORLD_SOURCES}
        ios/IOSMain.mm ios/IOSMedia.mm)
    set(MIKUMIKUWORLD_TARGET MikuMikuWorld)
else()
    add_executable(MikuMikuWorld ${MIKUMIKUWORLD_SOURCES})
    set(MIKUMIKUWORLD_TARGET MikuMikuWorld)
endif()

target_include_directories(${MIKUMIKUWORLD_TARGET} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/Depends/DirectXMath-master/Inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/Depends/glad/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/Depends/GLFW/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/Depends/stb_image"
    "${CMAKE_CURRENT_SOURCE_DIR}/Depends/miniaudio"
    "${CMAKE_CURRENT_SOURCE_DIR}/Depends/json"
    "${CMAKE_CURRENT_SOURCE_DIR}/Depends/stb_vorbis"
    "${CMAKE_CURRENT_SOURCE_DIR}/Depends/zlib/include"
)

target_compile_definitions(${MIKUMIKUWORLD_TARGET} PRIVATE
    MIKUMIKUWORLD_VERSION="${MIKUMIKUWORLD_VERSION_STRING}"
)
if(MIKUMIKUWORLD_SEKAI_LIVE_RENDERING)
    target_compile_definitions(${MIKUMIKUWORLD_TARGET} PRIVATE MIKUMIKUWORLD_SEKAI_LIVE_RENDERING)
endif()

if(MSVC)
    target_compile_options(${MIKUMIKUWORLD_TARGET} PRIVATE /utf-8)
    target_compile_definitions(${MIKUMIKUWORLD_TARGET} PRIVATE
        _CRT_SECURE_NO_WARNINGS
        _ENABLE_EXTENDED_ALIGNED_STORAGE
    )
else()
    # DirectXMath needs a SAL shim on Android/macOS/iOS. Keep it outside
    # DirectXMath's own include folder so MSVC can use the full SDK header.
    target_include_directories(${MIKUMIKUWORLD_TARGET} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/Depends/compat"
    )
    target_compile_options(${MIKUMIKUWORLD_TARGET} PRIVATE
        -Wno-deprecated-declarations
        -Wno-unknown-pragmas
    )
endif()

if(ANDROID)
    target_compile_definitions(${MIKUMIKUWORLD_TARGET} PRIVATE IMGUI_IMPL_OPENGL_ES3 MIKUMIKUWORLD_ANDROID _FILE_OFFSET_BITS=64)
    target_compile_options(${MIKUMIKUWORLD_TARGET} PRIVATE -fexceptions -frtti -Wno-error=format-security)
    target_include_directories(${MIKUMIKUWORLD_TARGET} PRIVATE "${CMAKE_ANDROID_NDK}/sources/android/native_app_glue")
    set(FFMPEG_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/android/ffmpeg-sdk/include")
    set(FFMPEG_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/android/ffmpeg-sdk/include-generated")
    set(FFMPEG_JNILIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/android/app/src/main/jniLibs/${ANDROID_ABI}")
    target_include_directories(${MIKUMIKUWORLD_TARGET} PRIVATE ${FFMPEG_INCLUDE_DIR} ${FFMPEG_GENERATED_INCLUDE_DIR})
    foreach(FFMPEG_COMPONENT avutil avcodec avformat swscale)
        add_library(ffmpeg_${FFMPEG_COMPONENT} SHARED IMPORTED)
        set_target_properties(ffmpeg_${FFMPEG_COMPONENT} PROPERTIES
            IMPORTED_LOCATION "${FFMPEG_JNILIB_DIR}/lib${FFMPEG_COMPONENT}.so"
        )
    endforeach()
    target_link_libraries(${MIKUMIKUWORLD_TARGET} PRIVATE
        android log EGL GLESv3 z
        ffmpeg_avformat ffmpeg_avcodec ffmpeg_swscale ffmpeg_avutil
    )
elseif(MIKUMIKUWORLD_IOS)
    set_source_files_properties(
        "${CMAKE_CURRENT_SOURCE_DIR}/MikuMikuWorld/Audio/AudioManager.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/MikuMikuWorld/Audio/Sound.cpp"
        PROPERTIES LANGUAGE OBJCXX)
    target_compile_definitions(MikuMikuWorld PRIVATE
        MIKUMIKUWORLD_IOS IMGUI_IMPL_OPENGL_ES3 GLFW_INCLUDE_NONE
        GL_SILENCE_DEPRECATION GLES_SILENCE_DEPRECATION)
    target_compile_options(MikuMikuWorld PRIVATE "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
    set_target_properties(MikuMikuWorld PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/ios/Info.plist.in"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.mm4ios.youkai"
        MACOSX_BUNDLE_BUNDLE_VERSION "1"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
        XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO"
        XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "NO"
        XCODE_ATTRIBUTE_ENABLE_BITCODE "NO"
        XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME "AppIcon")
    target_sources(MikuMikuWorld PRIVATE ios/Assets.xcassets)
    set_source_files_properties(ios/Assets.xcassets PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    target_link_libraries(MikuMikuWorld PRIVATE z
        "-framework UIKit" "-framework Foundation" "-framework QuartzCore"
        "-framework OpenGLES" "-framework AVFoundation" "-framework AudioToolbox"
        "-framework CoreAudio" "-framework CoreMedia" "-framework CoreVideo"
        "-framework CoreGraphics" "-framework UniformTypeIdentifiers")
elseif(APPLE)
    enable_language(OBJC)
    enable_language(OBJCXX)
    set(MAC_APP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/MikuMikuWorld/mmw_icon.icns")

    set_target_properties(MikuMikuWorld PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "MikuMikuWorld"
        MACOSX_BUNDLE_ICON_FILE "mmw_icon.icns"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.crash5band.MikuMikuWorld"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in"
    )

    target_sources(${MIKUMIKUWORLD_TARGET} PRIVATE ${MAC_APP_ICON})
    set_source_files_properties(${MAC_APP_ICON} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")

    set_source_files_properties(
        "${CMAKE_CURRENT_SOURCE_DIR}/MikuMikuWorld/main.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/MikuMikuWorld/File.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/MikuMikuWorld/IO.cpp"
        PROPERTIES LANGUAGE OBJCXX
    )

    target_compile_definitions(${MIKUMIKUWORLD_TARGET} PRIVATE
        GL_SILENCE_DEPRECATION
    )

    find_package(OpenGL REQUIRED)
    find_package(ZLIB REQUIRED)
    find_package(glfw3 QUIET)

    if(NOT TARGET glfw)
        include(FetchContent)
        set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
        set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(
            glfw
            GIT_REPOSITORY https://github.com/glfw/glfw.git
            GIT_TAG 3.4
        )
        FetchContent_MakeAvailable(glfw)
    endif()

    target_link_libraries(${MIKUMIKUWORLD_TARGET} PRIVATE
        glfw
        OpenGL::GL
        ZLIB::ZLIB
        "-framework AppKit"
        "-framework Cocoa"
        "-framework IOKit"
        "-framework CoreVideo"
    )
elseif(WIN32)
    target_link_libraries(${MIKUMIKUWORLD_TARGET} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/Depends/GLFW/lib/glfw3.lib"
        "${CMAKE_CURRENT_SOURCE_DIR}/Depends/zlib/lib/zlibstatic.lib"
        opengl32
        Version
        Dwmapi
    )
endif()

if(NOT ANDROID)
    add_custom_command(TARGET ${MIKUMIKUWORLD_TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/MikuMikuWorld/res"
            "$<TARGET_FILE_DIR:${MIKUMIKUWORLD_TARGET}>/res"
    )
endif()

if(APPLE AND NOT MIKUMIKUWORLD_IOS)
    add_custom_command(TARGET MikuMikuWorld POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -f
            "$<TARGET_FILE_DIR:MikuMikuWorld>/app_config.json"
            "$<TARGET_FILE_DIR:MikuMikuWorld>/imgui_config.ini"
        COMMAND ${CMAKE_COMMAND} -E remove_directory "$<TARGET_FILE_DIR:MikuMikuWorld>/auto_save"
        COMMAND ${CMAKE_COMMAND} -E remove_directory "$<TARGET_FILE_DIR:MikuMikuWorld>/library"
        COMMAND /usr/bin/codesign --force --deep --sign - "$<TARGET_BUNDLE_DIR:MikuMikuWorld>"
        VERBATIM
    )
endif()
