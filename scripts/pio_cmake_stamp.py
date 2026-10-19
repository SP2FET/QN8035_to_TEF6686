# PlatformIO post-script: stop CMake from re-configuring on every build.
#
# The espidf builder (espressif32 platform, builder/frameworks/espidf.py,
# is_cmake_reconfigure_required) re-runs CMake whenever sdkconfig.<env>,
# sdkconfig.defaults, CMakeLists.txt, main/CMakeLists.txt or the framework dir
# is newer than CMakeCache.txt. CMake rewrites CMakeCache.txt only when its
# content changes, so after e.g. a menuconfig save that check never clears and
# every build pays a full configure (~2 min on this machine).
#
# Post-scripts run after the builder has re-configured (a failed configure
# exits before this), so the cache is current here: bump its mtime.
import os

Import("env")  # noqa: F821 (SCons global)

cache = os.path.join(env.subst("$BUILD_DIR"), "CMakeCache.txt")  # noqa: F821
if os.path.isfile(cache):
    os.utime(cache, None)
