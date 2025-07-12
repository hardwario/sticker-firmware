#
# Copyright (c) 2025 HARDWARIO a.s.
#
# SPDX-License-Identifier: Apache-2.0
#

add_subdirectory_ifdef(CONFIG_SENSOR sensor)
add_subdirectory_ifdef(CONFIG_W1 w1)
