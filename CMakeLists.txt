# Copyright (c) 2026 Golioth, Inc.
#
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

list(APPEND ZEPHYR_EXTRA_MODULES
  ${CMAKE_CURRENT_SOURCE_DIR}/modules/tikk-led-matrix
  ${CMAKE_CURRENT_SOURCE_DIR}/../deps/modules/lib/micro_ros_zephyr_module/modules/libmicroros
)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(tikk_liquid_microros)

target_sources(app PRIVATE
  src/main.c
  src/nrf52840_power.c
  src/pouch_setup.c
  src/credentials.c
  src/fw_update.c
)
