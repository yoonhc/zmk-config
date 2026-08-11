# SPDX-License-Identifier: MIT

if(CONFIG_ZMK_BEHAVIOR_CAPS_MULTI_ROLE)
  target_sources(app PRIVATE src/behaviors/behavior_caps_multi_role.c)
endif()
