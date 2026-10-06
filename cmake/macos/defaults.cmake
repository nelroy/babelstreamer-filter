# CMake macOS defaults module

include_guard(GLOBAL)

# Set empty codesigning team if not specified as cache variable
if(NOT CODESIGN_TEAM)
  set(CODESIGN_TEAM "" CACHE STRING "OBS code signing team for macOS" FORCE)

  # Set ad-hoc codesigning identity if not specified as cache variable
  if(NOT CODESIGN_IDENTITY)
    set(CODESIGN_IDENTITY "-" CACHE STRING "OBS code signing identity for macOS" FORCE)
  endif()
endif()

# Developer ID Installer identity used to sign the .pkg installer produced by
# create-package.cmake.in. Left empty by default (unsigned .pkg) since, unlike
# CODESIGN_IDENTITY, there's no ad-hoc equivalent for installer packages —
# notarization of the .pkg requires a real Developer ID Installer certificate.
if(NOT CODESIGN_INSTALLER_IDENTITY)
  set(
    CODESIGN_INSTALLER_IDENTITY
    ""
    CACHE STRING
    "Developer ID Installer identity for signing the .pkg (empty = unsigned)"
  )
endif()

include(xcode)

include(buildspec)

# Use Applications directory as default install destination
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  set(
    CMAKE_INSTALL_PREFIX
    "$ENV{HOME}/Library/Application Support/obs-studio/plugins"
    CACHE STRING
    "Default plugin installation directory"
    FORCE
  )
endif()

# Enable find_package targets to become globally available targets
set(CMAKE_FIND_PACKAGE_TARGETS_GLOBAL TRUE)
# Enable RPATH support for generated binaries
set(CMAKE_MACOSX_RPATH TRUE)
# Use RPATHs from build tree _in_ the build tree
set(CMAKE_BUILD_WITH_INSTALL_RPATH FALSE)
# Do not add default linker search paths to RPATH
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH FALSE)
# Use common bundle-relative RPATH for installed targets
set(CMAKE_INSTALL_RPATH "@executable_path/../Frameworks")
