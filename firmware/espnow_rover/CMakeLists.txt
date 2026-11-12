cmake_minimum_required(VERSION 3.16)
set(COMPONENTS main)
set(EXTRA_COMPONENT_DIRS
    "../../components/espnow_rtcm"
    "../../components/rtcm_filter"
    "../../components/lora_transport")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(espnow-rtcm-usb-rover)
