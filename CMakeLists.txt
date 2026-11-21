cmake_minimum_required(VERSION 3.10)
project(vial-move-plugin CXX C)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Definitions to disable GUI/X11/OpenGL/ALSA/CURL in JUCE
add_definitions(
    -DLINUX=1
    -DNDEBUG=1
    -DHEADLESS=1
    -DJUCE_HEADLESS=1
    -DJUCE_GUI_SUPPORT=0
    -DJUCE_AUDIOPROCESSOR_NO_GUI=1
    -DJUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1
    -DJUCE_STANDALONE_APPLICATION=0
    -DJucePlugin_Build_Standalone=0
    -DJucePlugin_Build_VST=0
    -DJucePlugin_Build_VST3=0
    -DJucePlugin_Build_AU=0
    -DJucePlugin_Build_RTAS=0
    -DJucePlugin_Build_AAX=0
    -DJUCE_USE_CURL=0
    -DJUCE_LOAD_CURL_SYMBOLS_LAZILY=0
    -DJUCE_WEB_BROWSER=0
)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64" OR CMAKE_SYSTEM_PROCESSOR MATCHES "arm64")
    add_definitions(-D__ARM_NEON=1 -D__ARM_NEON__=1)
    add_compile_options(-O3 -mcpu=cortex-a53 -ffast-math -fno-finite-math-only -fno-math-errno)
endif()

# Header search paths
include_directories(
    src/dsp
    src/dsp/JuceLibraryCode
    src/dsp/vial/JUCE/modules
    src/dsp/vial/src
    src/dsp/vial/src/common
    src/dsp/vial/src/common/wavetable
    src/dsp/vial/src/interface/look_and_feel
    src/dsp/vial/src/synthesis
    src/dsp/vial/src/synthesis/effects
    src/dsp/vial/src/synthesis/filters
    src/dsp/vial/src/synthesis/framework
    src/dsp/vial/src/synthesis/lookups
    src/dsp/vial/src/synthesis/modulators
    src/dsp/vial/src/synthesis/modules
    src/dsp/vial/src/synthesis/producers
    src/dsp/vial/src/synthesis/synth_engine
    src/dsp/vial/src/synthesis/utilities
    src/dsp/vial/third_party
    src/dsp/vial/third_party/concurrentqueue
    src/dsp/vial/third_party/json
    src/dsp/vial/third_party/kissfft
)

# JUCE module sources
set(JUCE_SOURCES
    src/dsp/vial/JUCE/modules/juce_core/juce_core.cpp
    src/dsp/vial/JUCE/modules/juce_events/juce_events.cpp
    src/dsp/vial/JUCE/modules/juce_audio_basics/juce_audio_basics.cpp
    src/dsp/vial/JUCE/modules/juce_audio_formats/juce_audio_formats.cpp
    src/dsp/vial/JUCE/modules/juce_data_structures/juce_data_structures.cpp
)

# Vial synthesis engine sources (unity build)
set(VIAL_SOURCES
    src/dsp/vial/src/unity_build/common.cpp
    src/dsp/vial/src/unity_build/synthesis.cpp
)

# Move Anything DSP Plugin wrapper
set(PLUGIN_SOURCES
    src/dsp/vial_plugin.cpp
)

# Combine all source files
set(ALL_SOURCES
    ${JUCE_SOURCES}
    ${VIAL_SOURCES}
    ${PLUGIN_SOURCES}
)

# Build shared library (dsp.so)
add_library(vial-move-plugin SHARED ${ALL_SOURCES})

# Target properties to output dsp.so directly
set_target_properties(vial-move-plugin PROPERTIES
    PREFIX ""
    SUFFIX ".so"
    OUTPUT_NAME "dsp"
)

# Link standard libraries
target_link_libraries(vial-move-plugin PRIVATE
    pthread
    rt
    dl
)

# Link libstdc++ and libgcc statically to avoid GLIBCXX version issues on the Move,
# ensure no unresolved symbols, and hide all internal C++/JUCE symbols so Vial can
# coexist with Helm (JUCE 5) in the same process without symbol collisions.
target_link_options(vial-move-plugin PRIVATE
    "-static-libstdc++"
    "-static-libgcc"
    "-Wl,--no-undefined"
    "-Wl,-Bsymbolic"
    "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/dsp/plugin.lds"
)
