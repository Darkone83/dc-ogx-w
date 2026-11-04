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
