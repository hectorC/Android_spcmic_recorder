cmake_minimum_required(VERSION 3.22.1)

project("spcmic_recorder")

# C++17 standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Optimization flags
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -ffast-math -DNDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -O0 -g")

# Recording library source files (original)
set(RECORDING_SOURCES
    src/main/cpp/native-lib.cpp
    src/main/cpp/usb_audio_interface.cpp
    src/main/cpp/multichannel_recorder.cpp
    src/main/cpp/wav_writer.cpp
    src/main/cpp/uac_protocol.cpp
)

# Playback library source files (new)
set(PLAYBACK_SOURCES
    src/main/cpp/jni/playback_jni.cpp
    src/main/cpp/playback/wav_file_reader.cpp
    src/main/cpp/playback/playback_engine.cpp
    src/main/cpp/playback/audio_output.cpp
    src/main/cpp/playback/stereo_downmix.cpp
    src/main/cpp/wav_writer.cpp
    src/main/cpp/matrix_convolver/ir_loader.cpp
    src/main/cpp/matrix_convolver/matrix_convolver.cpp
    src/main/cpp/matrix_convolver/fft_engine.cpp
)

# Create recording library (original - keeps existing functionality)
add_library(${CMAKE_PROJECT_NAME} SHARED ${RECORDING_SOURCES})

# Create playback library (new - separate library for playback)
add_library(spcmic_playback SHARED ${PLAYBACK_SOURCES})

# Find and link required libraries
find_library(log-lib log)
find_library(android-lib android)

# Link libraries for recording (original library)
target_link_libraries(${CMAKE_PROJECT_NAME}
    ${log-lib}
    ${android-lib}
    OpenSLES
)

# Link libraries for playback
target_link_libraries(spcmic_playback
    ${log-lib}
    ${android-lib}
    OpenSLES
)

# Include directories
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    src/main/cpp
    src/main/cpp/playback
    src/main/cpp/jni
)

target_include_directories(spcmic_playback PRIVATE
    src/main/cpp
    src/main/cpp/playback
    src/main/cpp/jni
)
