# SPDX-License-Identifier: GPL-2.0-or-later
# Resolve dependency versions from the pinned Blender release, not a second version list.
include("${BLENDER_SOURCE}/build_files/build_environment/cmake/versions.cmake")
set(lock "{}")
foreach(name IN LISTS DEPENDENCIES)
  set(entry "{}")
  foreach(field VERSION URI HASH HASH_TYPE FILE)
    string(JSON entry SET "${entry}" "${field}" "\"${${name}_${field}}\"")
  endforeach()
  string(JSON lock SET "${lock}" "${name}" "${entry}")
endforeach()
file(WRITE "${OUTPUT}" "${lock}\n")
