# ##############################################################################
# vendor/HelmOne/boards/sf32lb52/helmone/ui/CMakeLists.txt
#
# Layer: product UI (bicycle + UI message queue).
#
# SPDX-License-Identifier: Apache-2.0
# ##############################################################################

if(CONFIG_MYVENDOR_BICYCLE)
  target_sources(board PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/myvendor_bicycle_ctl.c)
endif()

add_subdirectory(bicycle)
