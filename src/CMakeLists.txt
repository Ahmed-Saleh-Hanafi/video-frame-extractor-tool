cmake_minimum_required(VERSION 3.16)
project(VideoFrameExtractor)

set(CMAKE_CXX_STANDARD 17)

# ── OpenMP ────────────────────────────────────────────────────────────────────
find_package(OpenMP REQUIRED)

# ── OpenCV ────────────────────────────────────────────────────────────────────
# Set this to your OpenCV build directory if CMake can't find it automatically.
# Example: set(OpenCV_DIR "C:/opencv/build")
find_package(OpenCV REQUIRED COMPONENTS core imgcodecs videoio)

# ── Executable ────────────────────────────────────────────────────────────────
add_executable(VideoFrameExtractor WIN32
    main.cpp
)

target_include_directories(VideoFrameExtractor PRIVATE ${OpenCV_INCLUDE_DIRS})

target_link_libraries(VideoFrameExtractor
    PRIVATE
        ${OpenCV_LIBS}
        OpenMP::OpenMP_CXX
        comctl32   # for progress bar (InitCommonControlsEx)
        comdlg32   # for folder browser
        shell32
        ole32
)

# Copy OpenCV DLLs next to the executable after build (Release config)
add_custom_command(TARGET VideoFrameExtractor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E echo
        "Build done. Copy OpenCV DLLs to the output folder if needed."
)
