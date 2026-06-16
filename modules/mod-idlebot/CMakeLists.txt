# mod-idlebot — AzerothCore module CMake
#
# AzerothCore's module loader globs modules/*/CMakeLists.txt and the src tree.
# For most modules this file can be minimal — AC's CollectSourceFiles / module
# macros handle the heavy lifting. Verify against your AC version's module
# convention (other modules in modules/ are the reference).
#
# TODO(verify): Confirm this matches the CMake pattern used by other working
# modules in your AzerothCore checkout (e.g. modules/mod-eluna or any installed
# module). Some AC versions want AC_ADD_SCRIPTS / CU_*; copy from a known-good
# sibling module if this does not pick up sources.

# Collect this module's source files (recurses src/).
CU_GET_SOURCES(sources_localdir)

# YAML parsing for guide files. yaml-cpp is the common choice and is small.
# TODO(verify): decide how to vendor/link yaml-cpp inside the AC build.
# Options, in order of preference for a Docker build:
#   1. apt install libyaml-cpp-dev in the worldserver build image, then find_package.
#   2. add_subdirectory a vendored copy under deps/.
# Leaving find_package commented until the user confirms how deps are managed.
#
# find_package(yaml-cpp REQUIRED)

# Register with AzerothCore's script system.
# TODO(verify): the exact macro name varies by AC version — AddSC convention,
# or AC_ADD_SCRIPT_LOADER, etc. Match the sibling modules.
