# Adapt older Pico SDK target source lists to the pinned BTstack fork without
# editing anything in PICO_SDK_PATH. SDK 2.3 already supplies these source-list
# changes and the corrected CYW43 transport. Earlier SDKs use the small,
# BSD-licensed transport copy documented in sdk_compat/README.md.
foreach(_required pico_btstack_base pico_btstack_ble pico_btstack_hci_transport_cyw43 pico_cyw43_arch_lwip_threadsafe_background)
    if(NOT TARGET ${_required})
        message(FATAL_ERROR "Missing SDK target ${_required}; initialize the selected Pico SDK's wireless submodules")
    endif()
endforeach()

get_target_property(_ble_sources pico_btstack_ble INTERFACE_SOURCES)
list(TRANSFORM _ble_sources REPLACE "/hids_client\\.c$" "/hids_host.c")
set_property(TARGET pico_btstack_ble PROPERTY INTERFACE_SOURCES "${_ble_sources}")

get_target_property(_base_sources pico_btstack_base INTERFACE_SOURCES)
set(_event_builder "${PICO_BTSTACK_PATH}/src/hci_event_builder.c")
if(NOT _event_builder IN_LIST _base_sources AND NOT _event_builder IN_LIST _ble_sources)
    target_sources(pico_btstack_base INTERFACE "${_event_builder}")
endif()

if(PICO_SDK_VERSION_STRING VERSION_LESS "2.3.0")
    get_target_property(_transport_sources pico_btstack_hci_transport_cyw43 INTERFACE_SOURCES)
    set(_replaced FALSE)
    set(_new_transport_sources)
    foreach(_source IN LISTS _transport_sources)
        if(_source MATCHES "/btstack_hci_transport_cyw43\\.c$")
            list(APPEND _new_transport_sources "${CMAKE_CURRENT_LIST_DIR}/sdk_compat/btstack_hci_transport_cyw43.c")
            set(_replaced TRUE)
        else()
            list(APPEND _new_transport_sources "${_source}")
        endif()
    endforeach()
    if(NOT _replaced)
        message(FATAL_ERROR "SDK CYW43 transport layout changed; review cmake/sdk_compat.cmake")
    endif()
    set_property(TARGET pico_btstack_hci_transport_cyw43 PROPERTY INTERFACE_SOURCES "${_new_transport_sources}")
endif()

# Confirm the selected SDK or bundled shim has the upstream CYW43 HCI
# pre-buffer fix. SDK 2.3.0+ already supplies it; older SDKs use the copy
# selected above. Fail clearly if a custom SDK regresses this requirement.
include("${CMAKE_CURRENT_LIST_DIR}/verify_upstream_bluetooth.cmake")
get_target_property(_dc_hci_sources pico_btstack_hci_transport_cyw43 INTERFACE_SOURCES)
set(_dc_verified_hci_transport FALSE)
foreach(_dc_hci_source IN LISTS _dc_hci_sources)
    if(_dc_hci_source MATCHES "btstack_hci_transport_cyw43\\.c$")
        dc_ogx_verify_cyw43_transport("${_dc_hci_source}")
        set(_dc_verified_hci_transport TRUE)
    endif()
endforeach()
if(NOT _dc_verified_hci_transport)
    message(FATAL_ERROR
        "Bluetooth baseline: could not verify the selected CYW43 HCI transport source")
endif()
message(STATUS "dc-ogx-w: CYW43 HCI receive-buffer compatibility verified")

# Prove SDK actually compiles the build-owned, patched HIDS host.  A correct
# .diff on disk is meaningless if the SDK links another BTstack copy.
if(DC_OGX_HAS_BLUETOOTH)
    if(NOT "${PICO_BTSTACK_PATH}" STREQUAL "${DC_OGX_BTSTACK_SOURCE_DIR}")
        message(FATAL_ERROR "Bluetooth stack mismatch: selected ${PICO_BTSTACK_PATH}, patched ${DC_OGX_BTSTACK_SOURCE_DIR}")
    endif()
    get_target_property(_dc_bt_ble_sources pico_btstack_ble INTERFACE_SOURCES)
    set(_dc_required_hids_source "${DC_OGX_BTSTACK_SOURCE_DIR}/src/ble/gatt-service/hids_host.c")
    if(NOT _dc_required_hids_source IN_LIST _dc_bt_ble_sources)
        message(FATAL_ERROR "Pico SDK Bluetooth target is not using patched HIDS host: ${_dc_required_hids_source}")
    endif()
    message(STATUS "dc-ogx-w: confirmed compiled HIDS host: ${_dc_required_hids_source}")
endif()
