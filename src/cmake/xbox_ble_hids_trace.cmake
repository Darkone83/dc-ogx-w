# Passive BTstack HIDS diagnostics for dc-ogx-w. Do not change HID/GATT
# behavior. Called automatically by cmake/dependencies.cmake after the
# OGX-Mini 2026 HID Control Point correction was applied.
#
# The source is recreated from pinned BTstack revision 79c970b... before this
# transform. Every anchor is checked; do not silently skip mismatched changes.
function(dc_ogx_apply_hids_diagnostics btstack_tree)
    set(_hids_source "${btstack_tree}/src/ble/gatt-service/hids_host.c")
    if(NOT EXISTS "${_hids_source}")
        message(FATAL_ERROR "Missing BTstack HIDS source: ${_hids_source}")
    endif()
    file(READ "${_hids_source}" _hids_diag_text)
    if(_hids_diag_text MATCHES "dc_ogx_hids_trace_event")
        message(FATAL_ERROR "Expected a pristine pinned HIDS source before diagnostics overlay")
    endif()

    function(_dc_hids_replace old replacement label)
        string(FIND "${_hids_diag_text}" "${old}" _at)
        if(_at EQUAL -1)
            message(FATAL_ERROR "BTstack HIDS diagnostic insertion failed at ${label}: pinned source differs")
        endif()
        string(REPLACE "${old}" "${replacement}" _hids_diag_text "${_hids_diag_text}")
        set(_hids_diag_text "${_hids_diag_text}" PARENT_SCOPE)
    endfunction()

    _dc_hids_replace([=[
static void hids_host_emit_connection_established(hids_host_t * client, uint8_t status){
]=] [=[
/* Events are read-only and delivered on the existing Bluetooth run loop. */
extern void dc_ogx_hids_trace_event(uint16_t con_handle, uint8_t event, uint16_t detail);

static void hids_host_emit_connection_established(hids_host_t * client, uint8_t status){
    dc_ogx_hids_trace_event(client->con_handle, 1, status);
]=] "HIDS connection result")

    _dc_hids_replace([=[
    uint8_t report_index = find_report_index_for_value_handle(client, gatt_event_notification_get_value_handle(packet));
    if (report_index == HIDS_HOST_INVALID_REPORT_INDEX){
        return;
    }
]=] [=[
    uint8_t report_index = find_report_index_for_value_handle(client, gatt_event_notification_get_value_handle(packet));
    if (report_index == HIDS_HOST_INVALID_REPORT_INDEX){
        return;
    }
    dc_ogx_hids_trace_event(client->con_handle, 4, client->reports[report_index].report_id);
]=] "HIDS notification")

    _dc_hids_replace([=[
    uint8_t report_index = find_report_index_for_value_handle(client, gatt_event_characteristic_value_query_result_get_value_handle(packet));
    if (report_index == HIDS_HOST_INVALID_REPORT_INDEX){
        return;
    }
]=] [=[
    uint8_t report_index = find_report_index_for_value_handle(client, gatt_event_characteristic_value_query_result_get_value_handle(packet));
    if (report_index == HIDS_HOST_INVALID_REPORT_INDEX){
        return;
    }
    dc_ogx_hids_trace_event(client->con_handle, 5, client->reports[report_index].report_id);
]=] "HIDS GET reply")

    _dc_hids_replace([=[
            att_status = gatt_client_write_client_characteristic_configuration(&handle_gatt_client_event, client->con_handle, &characteristic, GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION);
]=] [=[
            att_status = gatt_client_write_client_characteristic_configuration(&handle_gatt_client_event, client->con_handle, &characteristic, GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION);
            dc_ogx_hids_trace_event(client->con_handle, 2, att_status);
]=] "CCCD submission")

    _dc_hids_replace([=[
                case HIDS_HOST_STATE_W4_INPUT_REPORTS_ENABLED:
                    if (hids_host_report_next_notification_report_index(client)){
]=] [=[
                case HIDS_HOST_STATE_W4_INPUT_REPORTS_ENABLED:
                    dc_ogx_hids_trace_event(client->con_handle, 3, status);
                    if (hids_host_report_next_notification_report_index(client)){
]=] "CCCD acknowledgment")

    _dc_hids_replace([=[
uint8_t hids_host_send_exit_suspend(uint16_t hids_cid, uint8_t service_index){
    return hids_host_send_control_point_cmd(hids_cid, service_index, 1);
}
]=] [=[
uint8_t hids_host_send_exit_suspend(uint16_t hids_cid, uint8_t service_index){
    uint8_t result = hids_host_send_control_point_cmd(hids_cid, service_index, 1);
    hids_host_t * client = hids_get_client_for_cid(hids_cid);
    if (client != NULL) dc_ogx_hids_trace_event(client->con_handle, 6, result);
    return result;
}
]=] "HIDS exit suspend")

    _dc_hids_replace([=[
uint8_t hids_host_send_get_report(uint16_t hids_cid, uint8_t report_id, hid_report_type_t report_type){
]=] [=[
static uint8_t dc_ogx_hids_get_report_impl(uint16_t hids_cid, uint8_t report_id, hid_report_type_t report_type){
]=] "GET report implementation")

    _dc_hids_replace([=[
uint8_t hids_host_get_hid_information(uint16_t hids_cid, uint8_t service_index){
]=] [=[
uint8_t hids_host_send_get_report(uint16_t hids_cid, uint8_t report_id, hid_report_type_t report_type){
    hids_host_t * client = hids_get_client_for_cid(hids_cid);
    uint8_t result = dc_ogx_hids_get_report_impl(hids_cid, report_id, report_type);
    if (client != NULL) dc_ogx_hids_trace_event(client->con_handle, 7, result);
    return result;
}

uint8_t hids_host_get_hid_information(uint16_t hids_cid, uint8_t service_index){
]=] "GET report request result")

    file(WRITE "${_hids_source}" "${_hids_diag_text}")
    message(STATUS "dc-ogx-w: passive HIDS notifications/GET/CCCD/Exit-Suspend diagnostics installed")
endfunction()
