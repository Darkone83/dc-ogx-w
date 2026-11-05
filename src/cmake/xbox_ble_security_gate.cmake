# dc-ogx-w: controlled Microsoft Xbox BLE security-order trial.
# Changes actual compiled Bluepad32 C source, not only build options.
# The fetched OGX-Mini 2026 dependency remains untouched; all other gamepads
# continue using the pinned 2026 BLE implementation.
# Xbox BLE is identified using 2026's pre-DIS heuristic, not post-DIS VID.
function(dc_ogx_prepare_xbox_ble_security upstream_dir output_dir upstream_rev)
    set(_patched "${output_dir}/src/components/bluepad32/bt/uni_bt_le.c")
    set(_stamp "${output_dir}/.dc_ogx_xbox_security_stamp")
    file(SHA256 "${CMAKE_CURRENT_FUNCTION_LIST_FILE}" _patch_sha)
    set(_desired_stamp "${upstream_rev}:${_patch_sha}")
    if(EXISTS "${_stamp}" AND EXISTS "${_patched}")
        file(READ "${_stamp}" _current_stamp)
        file(READ "${_patched}" _current_source)
        string(FIND "${_current_source}" "dc_ogx_ms_xbox_security_gate" _verified)
        if(_current_stamp STREQUAL _desired_stamp AND NOT _verified EQUAL -1)
            message(STATUS "dc-ogx-w: Xbox BLE security-gated source verified")
            return()
        endif()
    endif()

    # Start from the exact COMMITTED upstream revision. Never mutate
    # build/_deps/bluepad32-src and never inherit previous CMake experiments.
    # An earlier generated source can be read-only on Windows after extraction.
    # Make that file writable before deleting/recreating the build-owned tree.
    if(EXISTS "${_patched}")
        file(CHMOD "${_patched}" PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
    endif()
    file(REMOVE_RECURSE "${output_dir}")
    file(MAKE_DIRECTORY "${output_dir}")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/_deps")
    set(_snapshot "${CMAKE_CURRENT_BINARY_DIR}/_deps/dc-ogx-xbox-security-src.tar")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${upstream_dir}" archive
                --format=tar "--output=${_snapshot}" "${upstream_rev}"
        RESULT_VARIABLE _result ERROR_VARIABLE _error
    )
    if(NOT _result EQUAL 0 OR NOT EXISTS "${_snapshot}")
        message(FATAL_ERROR "Cannot restore pinned Bluepad32 revision: ${_error}")
    endif()
    file(ARCHIVE_EXTRACT INPUT "${_snapshot}" DESTINATION "${output_dir}")
    file(REMOVE "${_snapshot}")
    if(NOT EXISTS "${_patched}")
        message(FATAL_ERROR "Pinned Bluepad32 missing BLE implementation: ${_patched}")
    endif()
    file(READ "${_patched}" _source)

    # Validate exact pinned source fragments. A mismatch is not 'already applied'.
    macro(_dc_xbox_gate_replace old_var new_var context)
        string(FIND "${_source}" "${${old_var}}" _pos)
        if(_pos EQUAL -1)
            message(FATAL_ERROR "Xbox BLE security gate: unexpected upstream ${context}; no partial patch")
        endif()
        string(REPLACE "${${old_var}}" "${${new_var}}" _source "${_source}")
    endmacro()

    set(_before_helper [==[static void ogxm_sm_request_pairing(uni_hid_device_t* device,]==])
    set(_after_helper [==[/* Xbox LE must be recognized BEFORE DIS has returned 045e:0b13.
 * Reuse the exact 2026 discovery heuristic, including the anonymous BLE
 * gamepad fallback, rather than a name/VID-only test that misses this pad.
 * The old test could start HIDS before SMP completed while claiming to gate it. */
static bool dc_ogx_ms_xbox_security_gate(const uni_hid_device_t* d) {
    return d && d->conn.protocol == UNI_BT_CONN_PROTOCOL_BLE &&
           ogxm_device_is_xbox_ble(d);
}

static void ogxm_sm_request_pairing(uni_hid_device_t* device,]==])
    _dc_xbox_gate_replace(_before_helper _after_helper "helper insertion")

    set(_old_pair [==[    if (ogxm_device_is_switch2(device) || ogxm_device_is_xbox_ble(device)) {]==])
    set(_new_pair [==[    if (dc_ogx_ms_xbox_security_gate(device)) {
        /* The original Wired Opposite Pico W path requests bonding.
         * The 2026 pre-DIS heuristic also matches anonymous BLE gamepads,
         * so this test covers Xbox-compatible devices detected that way. */
        sm_set_authentication_requirements(SM_AUTHREQ_BONDING);
        logi("Xbox BLE: bonded SMP; waiting for pairing complete before HOGP\n");
    } else if (ogxm_device_is_switch2(device) || ogxm_device_is_xbox_ble(device)) {]==])
    _dc_xbox_gate_replace(_old_pair _new_pair "initial SMP authentication")

    set(_old_existing_bond [==[            if (device && ogxm_device_is_xbox_ble(device)) {]==])
    set(_new_existing_bond [==[            if (device && ogxm_device_is_xbox_ble(device) &&
                !dc_ogx_ms_xbox_security_gate(device)) {]==])
    # Two instances: re-encryption STARTED and COMPLETED.
    string(REGEX MATCHALL "            if \\(device && ogxm_device_is_xbox_ble\\(device\\)\\) \\{" _bond_matches "${_source}")
    list(LENGTH _bond_matches _bond_count)
    if(NOT _bond_count EQUAL 2)
        message(FATAL_ERROR "Xbox BLE gate: expected 2 prior-bond reset paths, found ${_bond_count}")
    endif()
    string(REPLACE "${_old_existing_bond}" "${_new_existing_bond}" _source "${_source}")

    set(_old_bond_mismatch [==[                    } else if (device && ogxm_device_is_xbox_ble(device)) {
                        logi("Xbox BLE: Just Works pair after bond mismatch\n");
                        sm_set_authentication_requirements(0);
                        sm_request_pairing(con_handle);]==])
    set(_new_bond_mismatch [==[                    } else if (device && ogxm_device_is_xbox_ble(device)) {
                        /* Stale Microsoft bond: re-pair WITH identity exchange. */
                        if (dc_ogx_ms_xbox_security_gate(device)) {
                            sm_set_authentication_requirements(SM_AUTHREQ_BONDING);
                            logi("Xbox BLE: stale key; retry bonded SMP\n");
                        } else {
                            sm_set_authentication_requirements(0);
                            logi("Xbox BLE: Just Works pair after bond mismatch\n");
                        }
                        sm_request_pairing(con_handle);]==])
    _dc_xbox_gate_replace(_old_bond_mismatch _new_bond_mismatch "stale-bond recovery")

    set(_old_bond_disconnect [==[                    if (device->vendor_id == 0x045e && s_le_reencryption_succeeded) {]==])
    set(_new_bond_disconnect [==[                    if (device->vendor_id == 0x045e && s_le_reencryption_succeeded &&
                        !dc_ogx_ms_xbox_security_gate(device)) {]==])
    _dc_xbox_gate_replace(_old_bond_disconnect _new_bond_disconnect "forced bond deletion on DIS")

    set(_old_early_hogp [==[    /*
     * Some Xbox-compatible BLE pads (SCUF Instinct empty-name) encrypt after Just
     * Works without ever emitting SM_EVENT_PAIRING_COMPLETE. Without that event]==])
    set(_new_early_hogp [==[    /* For Xbox-class BLE, ENCRYPTION_CHANGE can precede SMP's key
     * distribution and PAIRING_COMPLETE. Starting HIDS early creates a false
     * READY: GET works but the Xbox LED keeps blinking and it never notifies.
     * Wait for successful SM_EVENT_PAIRING_COMPLETE or REENCRYPTION_COMPLETE.
     * Other controllers keep the 2026 encryption-change fallback. */
    if (dc_ogx_ms_xbox_security_gate(device)) {
        logi("Xbox BLE: encrypted; waiting for SMP key distribution before GATT\n");
        return;
    }

    /*
     * Some Xbox-compatible BLE pads (SCUF Instinct empty-name) encrypt after Just
     * Works without ever emitting SM_EVENT_PAIRING_COMPLETE. Without that event]==])
    _dc_xbox_gate_replace(_old_early_hogp _new_early_hogp "early-HIDS security gate")

    set(_old_pairing_lookup [==[            device = uni_hid_device_get_instance_for_address(addr);
            con_handle = sm_event_pairing_complete_get_handle(packet);
            if (!device) {]==])
    set(_new_pairing_lookup [==[            device = uni_hid_device_get_instance_for_address(addr);
            con_handle = sm_event_pairing_complete_get_handle(packet);
            /* Xbox Series may advertise a resolvable private address that
             * differs from the SMP identity address. Use the live HCI handle. */
            if (!device) {
                uni_hid_device_t* by_handle = uni_hid_device_get_instance_for_connection_handle(con_handle);
                if (dc_ogx_ms_xbox_security_gate(by_handle)) {
                    device = by_handle;
                    logi("Xbox BLE: SMP event matched by handle after RPA identity change\n");
                }
            }
            if (!device) {]==])
    _dc_xbox_gate_replace(_old_pairing_lookup _new_pairing_lookup "SMP privacy identity lookup")

    set(_old_timeout [==[                    logi("Pairing failed, timeout\n");
                    break;]==])
    set(_new_timeout [==[                    logi("Pairing failed, timeout\n");
                    /* Xbox-class HOGP must never stay half-connected after a
                     * failed security exchange: disconnect to allow fresh pair. */
                    if (dc_ogx_ms_xbox_security_gate(device)) {
                        logi("Xbox BLE: security timed out before READY; disconnecting\n");
                        hog_disconnect(con_handle);
                    }
                    break;]==])
    _dc_xbox_gate_replace(_old_timeout _new_timeout "SMP timeout handling")

    # Git-archive extraction may restore the upstream file without write
    # permission on Windows. Clear its read-only bit before replacing the
    # generated source. This runs automatically on normal Compile/configure.
    file(CHMOD "${_patched}" PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
    file(WRITE "${_patched}" "${_source}")
    file(READ "${_patched}" _confirmed)
    string(FIND "${_confirmed}" "Xbox BLE: encrypted; waiting for SMP key distribution before GATT" _gate_position)
    string(FIND "${_confirmed}" "sm_set_authentication_requirements(SM_AUTHREQ_BONDING);" _bond_position)
    if(_gate_position EQUAL -1 OR _bond_position EQUAL -1)
        message(FATAL_ERROR "Microsoft Xbox BLE security source was not generated correctly")
    endif()
    file(WRITE "${_stamp}" "${_desired_stamp}")
    message(STATUS "dc-ogx-w: Xbox BLE security gate uses upstream pre-DIS identity heuristic; waiting for SMP")
endfunction()
