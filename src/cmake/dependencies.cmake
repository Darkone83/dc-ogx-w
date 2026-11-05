include(FetchContent)

# CMake owns downloads under build/_deps. Standard FETCHCONTENT_SOURCE_DIR_*
# overrides can supply existing checkouts; patched copies never modify them.
function(dc_ogx_fetch name repository revision required_file)
    FetchContent_Declare(${name}
        GIT_REPOSITORY "${repository}"
        GIT_TAG "${revision}"
        GIT_SUBMODULES ""
        GIT_SHALLOW FALSE
        SOURCE_SUBDIR _dc_ogx_download_only
    )
    FetchContent_MakeAvailable(${name})
    if(NOT EXISTS "${${name}_SOURCE_DIR}/${required_file}")
        message(FATAL_ERROR "Missing ${name} source: ${${name}_SOURCE_DIR}/${required_file}")
    endif()
    set(${name}_SOURCE_DIR "${${name}_SOURCE_DIR}" PARENT_SCOPE)
endfunction()

if(DC_OGX_HAS_BLUETOOTH)
    dc_ogx_fetch(bluepad32 https://github.com/MegaCadeDev/bluepad32.git
        86d2cb86fba1a5174b0a312667320a6c8a6c2b2e src/components/bluepad32/CMakeLists.txt)
    # Match the OGX-Mini 2026 pinned BTstack revision exactly.
    set(DC_OGX_BTSTACK_REVISION "79c970b67d498c81f4ec4bb777a72da2531da8cb")
    dc_ogx_fetch(btstack https://github.com/MegaCadeDev/btstack.git
        "${DC_OGX_BTSTACK_REVISION}" src/bluetooth.h)
endif()
dc_ogx_fetch(tinyusb https://github.com/MegaCadeDev/tinyusb.git
    94fd88304851fd03c51a0666f318540aae550655 src/tusb.c)
dc_ogx_fetch(libfixmath https://github.com/PetteriAimonen/libfixmath.git
    d308e466e1a09118d03f677c52e5fbf402f6fdd0 CMakeLists.txt)

if(DC_OGX_USB_HOST_ENABLED)
    # Use the locally installed PIO-USB checkout if supplied; otherwise fetch it.
    # The experimental build currently follows upstream main. Pin to the exact
    # working commit once verified with the user's Pico SDK/TinyUSB fork.
    if(PICO_PIO_USB_PATH)
        if(NOT EXISTS "${PICO_PIO_USB_PATH}/src/pio_usb.c")
            message(FATAL_ERROR "PICO_PIO_USB_PATH must contain src/pio_usb.c")
        endif()
    else()
        dc_ogx_fetch(pio_usb https://github.com/wiredopposite/Pico-PIO-USB.git
            main src/pio_usb.c)
        set(PICO_PIO_USB_PATH "${pio_usb_SOURCE_DIR}")
    endif()
endif()

# Exactly one local compatibility patch: OGX-Mini 2026's BTstack HIDS
# control-point scheduling fix.  Do NOT modify a fetched dependency in place.
# Every CMake configure recreates the build-owned BTstack copy from the pinned
# upstream commit.  This prevents a previous experimental patch from persisting.
function(dc_ogx_apply_hids_control_point btstack_tree)
    set(_file "${btstack_tree}/src/ble/gatt-service/hids_host.c")
    set(_patch "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/patches/btstack_hids_control_point.diff")
    if(NOT EXISTS "${_file}" OR NOT EXISTS "${_patch}")
        message(FATAL_ERROR "HIDS patch source or patch missing: ${_file}; ${_patch}")
    endif()

    # Scope both checks to the Control Point handler, not the neighbouring
    # set-protocol-mode handler (which already uses the correct scheduling call).
    set(_control_point_prefix
        "    client->handle = client->services[client->service_index].control_point_value_handle;\n    client->value = value;\n\n    client->state = HIDS_HOST_W2_WRITE_VALUE_OF_CHARACTERISTIC_WITHOUT_RESPONSE;\n    ")
    set(_bug "${_control_point_prefix}hids_host_request_to_send_next_query(client);")
    set(_fix "${_control_point_prefix}hids_host_request_to_send_write_without_response(client);")
    file(READ "${_file}" _contents)
    string(FIND "${_contents}" "${_fix}" _has_fix)
    string(FIND "${_contents}" "${_bug}" _has_bug)
    if(_has_fix GREATER_EQUAL 0 AND _has_bug EQUAL -1)
        message(STATUS "dc-ogx-w: BTstack HIDS control point already corrected")
    elseif(_has_bug GREATER_EQUAL 0 AND _has_fix EQUAL -1)
        # Same patch as OGX-Mini 2026.  If git's hunk context drifts, apply
        # exactly the verified block replacement, as upstream's script does.
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" apply --ignore-whitespace "${_patch}"
            WORKING_DIRECTORY "${btstack_tree}"
            RESULT_VARIABLE _result
            ERROR_VARIABLE _error
        )
        if(NOT _result EQUAL 0)
            message(STATUS "dc-ogx-w: git apply context missed; using exact upstream HIDS block replacement")
            string(REPLACE "${_bug}" "${_fix}" _contents "${_contents}")
            file(WRITE "${_file}" "${_contents}")
        endif()
    else()
        message(FATAL_ERROR
            "Unrecognized BTstack HID control-point implementation in ${_file}. "
            "Check the dependency revision instead of silently building the wrong path.")
    endif()

    file(READ "${_file}" _verified)
    string(FIND "${_verified}" "${_fix}" _verified_fix)
    string(FIND "${_verified}" "${_bug}" _verified_bug)
    if(_verified_fix EQUAL -1 OR NOT _verified_bug EQUAL -1)
        message(FATAL_ERROR "Xbox BLE HIDS control-point fix is NOT active in ${_file}")
    endif()
    message(STATUS "dc-ogx-w: verified upstream HIDS control-point fix in ${_file}")
endfunction()

if(DC_OGX_HAS_BLUETOOTH)
    set(DC_OGX_BLUEPAD32_SOURCE_DIR "${bluepad32_SOURCE_DIR}")
    set(DC_OGX_BTSTACK_SOURCE_DIR "${CMAKE_CURRENT_BINARY_DIR}/_deps/btstack-patched")

    # Important: FetchContent's btstack-src is a persistent Git worktree. Older
    # experimental patches can have changed its *working files*, even when the
    # checkout is still at the correct SHA. Simply file(COPY ...) from that
    # directory carries stale edits into the next build.
    #
    # Instead extract the exact COMMITTED source with git archive. This never
    # modifies the fetched source, and requires no manual git, scripts or
    # deletion of build folders. An archive is made only when the build-owned
    # copy is missing/stale; normal subsequent Compile clicks reuse it.
    set(_btstack_hids "${DC_OGX_BTSTACK_SOURCE_DIR}/src/ble/gatt-service/hids_host.c")
    set(_btstack_patch "${CMAKE_CURRENT_LIST_DIR}/patches/btstack_hids_control_point.diff")
    file(SHA256 "${_btstack_patch}" _btstack_patch_sha)
    set(_btstack_trace_module "${CMAKE_CURRENT_LIST_DIR}/xbox_ble_hids_trace.cmake")
    file(SHA256 "${_btstack_trace_module}" _btstack_trace_sha)
    set(_btstack_stamp_text "${DC_OGX_BTSTACK_REVISION}:${_btstack_patch_sha}:${_btstack_trace_sha}")
    set(_btstack_stamp_file "${DC_OGX_BTSTACK_SOURCE_DIR}/.dc_ogx_clean_btstack_stamp")
    set(_btstack_clean_copy_ok FALSE)
    if(EXISTS "${_btstack_stamp_file}" AND EXISTS "${_btstack_hids}")
        file(READ "${_btstack_stamp_file}" _existing_stamp)
        if(_existing_stamp STREQUAL _btstack_stamp_text)
            file(READ "${_btstack_hids}" _existing_hids)
            # Include the control-point handle to avoid matching the adjacent
            # already-correct protocol-mode write-without-response handler.
            set(_control_point_prefix
                "    client->handle = client->services[client->service_index].control_point_value_handle;\n    client->value = value;\n\n    client->state = HIDS_HOST_W2_WRITE_VALUE_OF_CHARACTERISTIC_WITHOUT_RESPONSE;\n    ")
            set(_expected_call "${_control_point_prefix}hids_host_request_to_send_write_without_response(client);")
            set(_broken_call "${_control_point_prefix}hids_host_request_to_send_next_query(client);")
            string(FIND "${_existing_hids}" "${_expected_call}" _has_correct_call)
            string(FIND "${_existing_hids}" "${_broken_call}" _has_wrong_call)
            string(FIND "${_existing_hids}" "dc_ogx_hids_trace_event(client->con_handle, 1, status)" _has_hids_trace)
            if(NOT _has_correct_call EQUAL -1 AND _has_wrong_call EQUAL -1 AND
               NOT _has_hids_trace EQUAL -1)
                set(_btstack_clean_copy_ok TRUE)
            endif()
        endif()
    endif()

    if(NOT _btstack_clean_copy_ok)
        message(STATUS "dc-ogx-w: restoring clean pinned BTstack and upstream HID fix")
        file(REMOVE_RECURSE "${DC_OGX_BTSTACK_SOURCE_DIR}")
        file(MAKE_DIRECTORY "${DC_OGX_BTSTACK_SOURCE_DIR}")
        set(_btstack_archive "${CMAKE_CURRENT_BINARY_DIR}/_deps/btstack-clean-snapshot.tar")
        # Archive the pinned commit, not the potentially modified working tree.
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" -C "${btstack_SOURCE_DIR}" archive
                    --format=tar "--output=${_btstack_archive}"
                    "${DC_OGX_BTSTACK_REVISION}" src platform 3rd-party tool
            RESULT_VARIABLE _archive_result ERROR_VARIABLE _archive_error
        )
        if(NOT _archive_result EQUAL 0 OR NOT EXISTS "${_btstack_archive}")
            message(FATAL_ERROR
                "Cannot extract pinned BTstack revision ${DC_OGX_BTSTACK_REVISION}: ${_archive_error}")
        endif()
        file(ARCHIVE_EXTRACT INPUT "${_btstack_archive}"
             DESTINATION "${DC_OGX_BTSTACK_SOURCE_DIR}")
        file(REMOVE "${_btstack_archive}")
        dc_ogx_apply_hids_control_point("${DC_OGX_BTSTACK_SOURCE_DIR}")
        include("${_btstack_trace_module}")
        dc_ogx_apply_hids_diagnostics("${DC_OGX_BTSTACK_SOURCE_DIR}")
        # Create the stamp only *after* both source changes are verified.
        file(WRITE "${_btstack_stamp_file}" "${_btstack_stamp_text}")
    else()
        message(STATUS "dc-ogx-w: clean patched BTstack already verified")
    endif()

    include("${CMAKE_CURRENT_LIST_DIR}/verify_upstream_bluetooth.cmake")
    dc_ogx_verify_bluetooth_baseline(
        "${DC_OGX_BLUEPAD32_SOURCE_DIR}" "${DC_OGX_BTSTACK_SOURCE_DIR}")

    # Xbox Series BLE security-order trial: source overlay generated from the
    # pinned Bluepad32 commit, never an old edited build/_deps worktree.
    # Runs in standard CMake regeneration; no extra preset or script.
    include("${CMAKE_CURRENT_LIST_DIR}/xbox_ble_security_gate.cmake")
    set(_dc_bluepad32_secure "${CMAKE_CURRENT_BINARY_DIR}/_deps/bluepad32-xbox-secure")
    dc_ogx_prepare_xbox_ble_security(
        "${bluepad32_SOURCE_DIR}" "${_dc_bluepad32_secure}"
        "86d2cb86fba1a5174b0a312667320a6c8a6c2b2e")
    # CMakeLists.txt will add_subdirectory this verified source directory.
    set(DC_OGX_BLUEPAD32_SOURCE_DIR "${_dc_bluepad32_secure}")
endif()
