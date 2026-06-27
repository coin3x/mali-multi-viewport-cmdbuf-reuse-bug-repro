# For more information about using CMake with Android Studio, read the
# documentation: https://d.android.com/studio/projects/add-native-code.html

cmake_minimum_required(VERSION 3.22.1)

project("repro")

find_package(Vulkan REQUIRED)

# Creates your game shared library. The name must be the same as the
# one used for loading in your Kotlin/Java or AndroidManifest.txt files.
add_library(repro SHARED
        main.cpp
        Renderer.cpp)

# Searches for a package provided by the game activity dependency
find_package(game-activity REQUIRED CONFIG)

# Forces the linker to keep the JNI entry point for GameActivity
set(CMAKE_SHARED_LINKER_FLAGS
        "${CMAKE_SHARED_LINKER_FLAGS} -u Java_com_google_androidgamesdk_GameActivity_initializeNativeCode")

target_include_directories(repro
        PRIVATE
        "${Vulkan_INCLUDE_DIR}"
)
# Configure libraries CMake uses to link your target library.
target_link_libraries(repro
        # The game activity
        game-activity::game-activity_static
        ${Vulkan_LIBRARIES}
        android
        log)