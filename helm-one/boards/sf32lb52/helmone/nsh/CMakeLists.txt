# ##############################################################################
# vendor/HelmOne/boards/sf32lb52/helmone/nsh/CMakeLists.txt
#
# Layer: NSH applets and probes.
#
# SPDX-License-Identifier: Apache-2.0
# ##############################################################################

add_subdirectory(test)
add_subdirectory(test_app)
add_subdirectory(ble_test_app)
add_subdirectory(lvgl_bench)
add_subdirectory(transfer_ui)

if(CONFIG_CRYPTO)
  nuttx_add_application(
    NAME
    md5_test
    STACKSIZE
    4096
    PRIORITY
    100
    SRCS
    md5_test.c)
endif()

nuttx_add_application(
  NAME
  ptab
  STACKSIZE
  2048
  PRIORITY
  100
  INCLUDE_DIRECTORIES
  ${NUTTX_BOARD_ABS_DIR}/include
  SRCS
  ptab_main.c)

nuttx_add_application(
  NAME
  boardmem
  STACKSIZE
  4096
  PRIORITY
  100
  INCLUDE_DIRECTORIES
  ${NUTTX_BOARD_ABS_DIR}/include
  SRCS
  boardmem_main.c
  DEPENDS
  board)

nuttx_add_application(
  NAME
  btnpeek
  STACKSIZE
  2048
  PRIORITY
  100
  INCLUDE_DIRECTORIES
  ${NUTTX_BOARD_ABS_DIR}/include
  SRCS
  btnpeek_main.c
  DEPENDS
  board)

nuttx_add_application(
  NAME
  mtpstat
  STACKSIZE
  2048
  PRIORITY
  100
  INCLUDE_DIRECTORIES
  ${NUTTX_BOARD_ABS_DIR}/include
  SRCS
  mtpstat_main.c
  DEPENDS
  board)

if(CONFIG_MYVENDOR_LCD_DISP)
  nuttx_add_application(
    NAME
    lcd_recover
    STACKSIZE
    4096
    PRIORITY
    100
    INCLUDE_DIRECTORIES
    ${NUTTX_BOARD_ABS_DIR}/include
    SRCS
    lcd_recover_main.c
    DEPENDS
    lvgl
    board)
endif()
