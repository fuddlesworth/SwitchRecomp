# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# swipc on its own: a cross build (cmake/swipc.cmake) builds it with this project for the machine it runs on
cmake_minimum_required(VERSION 3.25)
project(swipc LANGUAGES C)
add_executable(swipc swipc.c)
# C11 on a Mac, as the main build's strict C (SNX_C90_STANDARD): Apple's headers use long long
if(APPLE)
    set_target_properties(swipc PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
else()
    set_target_properties(swipc PROPERTIES C_STANDARD 90 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
endif()
