# CMake macOS helper functions module

include_guard(GLOBAL)

include(helpers_common)

# set_target_properties_obs: Set target properties for use in obs-studio
function(set_target_properties_plugin target)
  set(options "")
  set(oneValueArgs "")
  set(multiValueArgs PROPERTIES)
  cmake_parse_arguments(PARSE_ARGV 0 _STPO "${options}" "${oneValueArgs}" "${multiValueArgs}")

  message(DEBUG "Setting additional properties for target ${target}...")

  while(_STPO_PROPERTIES)
    list(POP_FRONT _STPO_PROPERTIES key value)
    set_property(TARGET ${target} PROPERTY ${key} "${value}")
  endwhile()

  string(TIMESTAMP CURRENT_YEAR "%Y")
  set_target_properties(
    ${target}
    PROPERTIES
      BUNDLE TRUE
      BUNDLE_EXTENSION plugin
      XCODE_ATTRIBUTE_PRODUCT_NAME ${target}
      XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${MACOS_BUNDLEID}
      XCODE_ATTRIBUTE_CURRENT_PROJECT_VERSION ${PLUGIN_BUILD_NUMBER}
      XCODE_ATTRIBUTE_MARKETING_VERSION ${PLUGIN_VERSION}
      XCODE_ATTRIBUTE_GENERATE_INFOPLIST_FILE YES
      XCODE_ATTRIBUTE_INFOPLIST_FILE ""
      XCODE_ATTRIBUTE_INFOPLIST_KEY_CFBundleDisplayName ${target}
      XCODE_ATTRIBUTE_INFOPLIST_KEY_NSHumanReadableCopyright "(c) ${CURRENT_YEAR} ${PLUGIN_AUTHOR}"
      XCODE_ATTRIBUTE_INSTALL_PATH "$(USER_LIBRARY_DIR)/Application Support/obs-studio/plugins"
  )

  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/macos/entitlements.plist")
    set_target_properties(
      ${target}
      PROPERTIES XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/macos/entitlements.plist"
    )
  endif()

  if(TARGET plugin-support)
    target_link_libraries(${target} PRIVATE plugin-support)
  elseif(PLUGIN_SUPPORT_SOURCES)
    target_sources(${target} PRIVATE ${PLUGIN_SUPPORT_SOURCES})
    target_include_directories(${target} PRIVATE "${PLUGIN_SUPPORT_INCLUDE_DIR}")
  endif()

  target_install_resources(${target})

  # data/tls/libqsecuretransportbackend.dylib (see ModelDownloadDialog.cpp's
  # ensureTlsBackendAvailable()) is already placed at Resources/tls/ by
  # target_install_resources() above, same as any other data/ file. But it's
  # executable code, not a passive resource — Qt's own TLS backend plugin,
  # bundled because OBS.app's Qt6 deployment ships QtNetwork.framework but
  # strips every TLS backend (it never does HTTPS via QNetworkAccessManager
  # itself), so any HTTPS request from inside the OBS process otherwise
  # fails with "TLS initialization failed". Being executable, it needs its
  # own code signature independent of the outer bundle's seal, or
  # dyld/Gatekeeper refuses to load it once notarized — the generic
  # Resources copy above doesn't do that for nested Mach-O files.
  # --timestamp requires a real (non-ad-hoc) identity, hence gating on
  # CODESIGN_TEAM the same way xcode.cmake does for the main target.
  set(_tls_backend_sign_flags "")
  if(CODESIGN_TEAM)
    set(_tls_backend_sign_flags --options runtime --timestamp)
  endif()

  add_custom_command(
    TARGET ${target}
    POST_BUILD
    COMMAND
      /usr/bin/codesign --force --sign "${CODESIGN_IDENTITY}" ${_tls_backend_sign_flags}
      "$<TARGET_BUNDLE_DIR:${target}>/Contents/Resources/tls/libqsecuretransportbackend.dylib"
    COMMENT "Signing bundled Qt TLS backend plugin"
    VERBATIM
  )

  add_custom_command(
    TARGET ${target}
    POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/rundir/$<CONFIG>"
    COMMAND
      "${CMAKE_COMMAND}" -E copy_directory "$<TARGET_BUNDLE_DIR:${target}>"
      "${CMAKE_CURRENT_BINARY_DIR}/rundir/$<CONFIG>/$<TARGET_BUNDLE_DIR_NAME:${target}>"
    COMMENT "Copy ${target} to rundir"
    VERBATIM
  )

  get_target_property(target_sources ${target} SOURCES)
  set(target_ui_files ${target_sources})
  list(FILTER target_ui_files INCLUDE REGEX ".+\\.(ui|qrc)")
  source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" PREFIX "UI Files" FILES ${target_ui_files})

  if(NOT PLUGIN_STANDALONE_XCODE)
    # These install() rules are what makes Xcode generate a synthetic
    # "install" target/scheme. Skip them for a standalone project, since it's
    # meant to be Archived and notarized directly from Xcode instead.
    install(TARGETS ${target} LIBRARY DESTINATION .)
    install(FILES "$<TARGET_BUNDLE_DIR:${target}>.dsym" CONFIGURATIONS Release DESTINATION . OPTIONAL)

    configure_file(cmake/macos/resources/distribution.in "${CMAKE_CURRENT_BINARY_DIR}/distribution" @ONLY)
    configure_file(
      cmake/macos/resources/create-package.cmake.in
      "${CMAKE_CURRENT_BINARY_DIR}/create-package.cmake"
      @ONLY
    )
    install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/create-package.cmake")
  endif()
endfunction()

# target_install_resources: Helper function to add resources into bundle
function(target_install_resources target)
  message(DEBUG "Installing resources for target ${target}...")
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/data")
    file(GLOB_RECURSE data_files "${CMAKE_CURRENT_SOURCE_DIR}/data/*")
    foreach(data_file IN LISTS data_files)
      cmake_path(
        RELATIVE_PATH
        data_file
        BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/data/"
        OUTPUT_VARIABLE relative_path
      )
      cmake_path(GET relative_path PARENT_PATH relative_path)
      target_sources(${target} PRIVATE "${data_file}")
      set_property(SOURCE "${data_file}" PROPERTY MACOSX_PACKAGE_LOCATION "Resources/${relative_path}")
      source_group("Resources/${relative_path}" FILES "${data_file}")
    endforeach()
  endif()
endfunction()

# target_add_resource: Helper function to add a specific resource to a bundle
function(target_add_resource target resource)
  message(DEBUG "Add resource ${resource} to target ${target} at destination ${destination}...")
  target_sources(${target} PRIVATE "${resource}")
  set_property(SOURCE "${resource}" PROPERTY MACOSX_PACKAGE_LOCATION Resources)
  source_group("Resources" FILES "${resource}")
endfunction()
