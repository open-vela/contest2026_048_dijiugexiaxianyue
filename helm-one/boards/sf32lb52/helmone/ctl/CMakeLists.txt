# ##############################################################################
# vendor/HelmOne/boards/sf32lb52/helmone/ctl/CMakeLists.txt
#
# Policy writes + NSH `ctl`. Reads are sys/.
#
# SPDX-License-Identifier: Apache-2.0
# ##############################################################################

target_sources(board PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/myvendor_devctl.c)
target_include_directories(board PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

nuttx_add_application(
  NAME
  ctl
  STACKSIZE
  4096
  PRIORITY
  100
  INCLUDE_DIRECTORIES
  ${NUTTX_BOARD_ABS_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}
  SRCS
  ctl_main.c
  DEPENDS
  board)
