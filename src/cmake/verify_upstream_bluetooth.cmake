# Verify the effective OGX-Mini 2026 Bluetooth compatibility baseline.
# This runs as part of normal CMake configuration; no developer script/preset.
# Dependencies remain pinned; do not reapply upstream patches that are already
# merged into those pinned sources. Refuse to silently build a regressed stack.

function(dc_ogx_require_source_fragment path needle description)
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "Bluetooth baseline: missing ${description}: ${path}")
    endif()
    file(READ "${path}" _dc_ogx_check_text)
    string(FIND "${_dc_ogx_check_text}" "${needle}" _dc_ogx_pos)
    if(_dc_ogx_pos EQUAL -1)
        message(FATAL_ERROR
            "Bluetooth baseline: ${description} is absent from ${path}. "
            "Reconfigure with the pinned dependencies; do not ship this build.")
    endif()
endfunction()

function(dc_ogx_verify_bluetooth_baseline bluepad32_path btstack_path)
    # Already merged into pinned BTstack 79c970b6 (2026 upstream).
    dc_ogx_require_source_fragment(
        "${btstack_path}/src/l2cap.c"
        "channel->state = L2CAP_STATE_WAIT_INCOMING_SECURITY_LEVEL_UPDATE;\n#if 0"
        "upstream L2CAP security compatibility correction")
    dc_ogx_require_source_fragment(
        "${btstack_path}/src/ble/gatt-service/hids_host.h"
        "#define HIDS_HOST_NUM_REPORTS 32"
        "32-report HID-host capacity")

    # Not merged into this pinned BTstack revision; patched during configure.
    dc_ogx_require_source_fragment(
        "${btstack_path}/src/ble/gatt-service/hids_host.c"
        "hids_host_request_to_send_write_without_response(client);"
        "Xbox BLE HID control-point scheduler correction")

    # Already merged into pinned Bluepad32 86d2cb86 (2026 upstream).
    dc_ogx_require_source_fragment(
        "${bluepad32_path}/src/components/bluepad32/include/uni.h"
        "#include \"parser/uni_hid_parser_ds4.h\""
        "Bluepad32 compatibility parser exports")
    dc_ogx_require_source_fragment(
        "${bluepad32_path}/src/components/bluepad32/include/controller/uni_controller_list.h"
        "MAKE_CONTROLLER_ID(0x2dc8, 0x6003)"
        "8BitDo device ID 0x6003")
    dc_ogx_require_source_fragment(
        "${bluepad32_path}/src/components/bluepad32/include/controller/uni_controller_list.h"
        "MAKE_CONTROLLER_ID(0x2dc8, 0x6103)"
        "8BitDo device ID 0x6103")

    message(STATUS "dc-ogx-w: OGX-Mini 2026 BTstack/Bluepad32 baseline verified")
endfunction()

# The selected CYW43 transport (SDK 2.3+ native, older SDK bundled shim)
# must preserve the incoming HCI pre-buffer.  This was an upstream fix for
# corrupted BLE/GATT packet reception on older Pico SDK versions.
function(dc_ogx_verify_cyw43_transport source_path)
    dc_ogx_require_source_fragment(
        "${source_path}"
        "HCI_INCOMING_PRE_BUFFER_SIZE_ALIGNED"
        "CYW43 aligned incoming HCI pre-buffer")
    dc_ogx_require_source_fragment(
        "${source_path}"
        "cyw43_receive_buffer"
        "CYW43 incoming HCI receive pointer")
endfunction()
