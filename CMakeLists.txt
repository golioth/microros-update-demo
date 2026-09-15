# Copyright (c) 2026 Golioth, Inc.
#
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# micro-ROS module location: default = the NCS workspace checkout next to
# this app. Set MICROROS_ZEPHYR_MODULE_PATH to the micro_ros_zephyr_module
# ROOT to build against another workspace (e.g. the pure-Zephyr 4.4 one);
# libmicroros (the Zephyr module) is a subdir of the root.
# NOTE: promicro_nrf52840 needs no BOARD_ROOT — upstream Zephyr 4.4
# carries the board at boards/others/promicro_nrf52840.
if(DEFINED ENV{MICROROS_ZEPHYR_MODULE_PATH})
  set(MICROROS_MODULE_ROOT $ENV{MICROROS_ZEPHYR_MODULE_PATH})
else()
  set(MICROROS_MODULE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../deps/modules/lib/micro_ros_zephyr_module)
endif()

list(APPEND ZEPHYR_EXTRA_MODULES
  ${CMAKE_CURRENT_SOURCE_DIR}/modules/tikk-led-matrix
  ${MICROROS_MODULE_ROOT}/modules/libmicroros
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
