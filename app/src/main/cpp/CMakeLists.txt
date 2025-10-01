cmake_minimum_required(VERSION 3.22.1)

project("spcmic_recorder")

# Configure C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find required packages
find_library(log-lib log)
find_library(android-lib android)
find_library(m-lib m)

# Add USB audio native library
add_library(spcmic_recorder
    SHARED
    native-lib.cpp
    usb_audio_interface.cpp
    uac_protocol.cpp
    multichannel_recorder.cpp
    wav_writer.cpp
)

# Include directories
target_include_directories(spcmic_recorder PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Link libraries
target_link_libraries(spcmic_recorder
    ${log-lib}
    ${android-lib}
    ${m-lib}
)