# SDK 2.3+ has modular linker scripts and a supported override API. Keep its
# startup, TLS and section layout, overriding only our stack/storage placement.
if(COMMAND pico_add_linker_script_override_path AND COMMAND pico_set_linker_script_var)
    pico_set_linker_script_var(dc-ogx-w RAM_LENGTH 245760) # 240 KiB; top 16 KiB are stacks.
    pico_add_linker_script_override_path(dc-ogx-w "${CMAKE_CURRENT_LIST_DIR}/linker"
        FILES memory_extra.incl sections_stack.incl section_extra_post_end.incl)
    message(STATUS "dc-ogx-w: modular linker overrides (8 KiB stacks, 1000 KiB OTA image, 24 KiB settings)")
else()
    # SDK 2.1/2.2 use the original single-file linker script.
    set(_sdk_linker "${PICO_SDK_PATH}/src/rp2_common/pico_crt0/rp2040/memmap_default.ld")
    if(NOT EXISTS "${_sdk_linker}")
        message(FATAL_ERROR
            "Cannot locate the SDK RP2040 linker script: ${_sdk_linker}. "
            "Use a complete Pico SDK 2.1/2.2 installation or SDK 2.3+ with modular linker support.")
    endif()
    set(_linker "${DC_OGX_GENERATED}/memmap_remote.ld")
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${DC_OGX_SRC}/NetworkService/configure_linker.py"
                "${_sdk_linker}" "${_linker}"
        RESULT_VARIABLE _linker_result
        ERROR_VARIABLE _linker_error
    )
    if(NOT _linker_result EQUAL 0)
        message(FATAL_ERROR
            "Cannot configure the 8 KiB stacks and protected flash region for this SDK: ${_linker_error}")
    endif()
    pico_set_linker_script(dc-ogx-w "${_linker}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${DC_OGX_SRC}/NetworkService/configure_linker.py" "${_sdk_linker}")
endif()

# Call after pico_add_extra_outputs(), which emits dc-ogx-w.bin first. Rebuilding
# the package generator relinks the target so a stale package cannot survive.
function(dc_ogx_add_update_package)
    set(_package_script "${CMAKE_CURRENT_SOURCE_DIR}/tools/firmware/make_update.py")
    set_property(TARGET dc-ogx-w APPEND PROPERTY LINK_DEPENDS "${_package_script}")
    add_custom_command(TARGET dc-ogx-w POST_BUILD
        COMMAND "${Python3_EXECUTABLE}" "${_package_script}"
            --board "${PICO_BOARD}"
            --input "$<TARGET_FILE_DIR:dc-ogx-w>/dc-ogx-w.bin"
            --output "$<TARGET_FILE_DIR:dc-ogx-w>/dc-ogx-w-${PICO_BOARD}.ogxw"
        BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/dc-ogx-w-${PICO_BOARD}.ogxw"
        COMMENT "Packaging checked dc-ogx-w firmware for the WebUI updater"
        VERBATIM)
endfunction()

set(_web_header "${DC_OGX_GENERATED}/ogxm_web_assets.h")
add_custom_command(OUTPUT "${_web_header}"
    COMMAND "${Python3_EXECUTABLE}" "${DC_OGX_SRC}/NetworkService/embed_web.py"
            "${DC_OGX_SRC}/NetworkService/web/index.html" "${_web_header}"
    DEPENDS "${DC_OGX_SRC}/NetworkService/embed_web.py" "${DC_OGX_SRC}/NetworkService/web/index.html"
    VERBATIM)
target_sources(dc-ogx-w PRIVATE "${_web_header}")
if(DC_OGX_HAS_BLUETOOTH)
set(_gatt_header "${DC_OGX_GENERATED}/att_delayed_response.h")
add_custom_command(OUTPUT "${_gatt_header}"
    COMMAND "${Python3_EXECUTABLE}" "${PICO_BTSTACK_PATH}/tool/compile_gatt.py"
            "${DC_OGX_SRC}/BLEServer/att_delayed_response.gatt" "${_gatt_header}"
    WORKING_DIRECTORY "${PICO_BTSTACK_PATH}"
    DEPENDS "${DC_OGX_SRC}/BLEServer/att_delayed_response.gatt" "${PICO_BTSTACK_PATH}/tool/compile_gatt.py"
    VERBATIM)
target_sources(dc-ogx-w PRIVATE "${_gatt_header}")
endif()
