cmake_minimum_required(VERSION 3.16)

# the linux target of ESP-IDF (the window on the computer): only main and what it needs, as not every component of
# ESP-IDF builds for that target
if(IDF_TARGET STREQUAL "linux")
    set(COMPONENTS main)
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(dgx_morph_word_demo)
