# Fix only the proven PIO-USB EOP polling race on the native Pico W.
# Reference: sekigon-gonnoc/Pico-PIO-USB PR #206 (commit 02ca10b),
# specifically the TX state-machine program-counter wait. The entire PR has
# not been merged upstream; do not import its unrelated changes here.
#
# This is invoked automatically by CMake before pico_sdk_init(), allowing the
# Pico SDK's existing tinyusb_pico_pio_usb target to consume the patched tree.
# The original FetchContent checkout remains unchanged.
function(dc_ogx_prepare_pio_usb_eop_fix source_root patched_root)
    if(NOT EXISTS "${source_root}/src/pio_usb.c")
        message(FATAL_ERROR "PIO-USB source is missing: ${source_root}/src/pio_usb.c")
    endif()
    get_filename_component(_source_root "${source_root}" REALPATH)
    get_filename_component(_patch_root "${patched_root}" ABSOLUTE)
    if(_source_root STREQUAL _patch_root)
        message(FATAL_ERROR "PIO-USB patched source and original checkout must be separate")
    endif()

    # A stamp avoids a full dependency copy on normal incremental builds.
    file(SHA256 "${source_root}/src/pio_usb.c" _source_sha)
    file(SHA256 "${CMAKE_CURRENT_FUNCTION_LIST_FILE}" _fix_sha)
    set(_stamp "${_source_sha}:${_fix_sha}")
    set(_stamp_path "${patched_root}/.dc_ogx_eop_fix_stamp")
    set(_need_update TRUE)
    if(EXISTS "${_stamp_path}" AND EXISTS "${patched_root}/src/pio_usb.c")
        file(READ "${_stamp_path}" _existing_stamp)
        if(_existing_stamp STREQUAL _stamp)
            set(_need_update FALSE)
        endif()
    endif()

    if(_need_update)
        file(REMOVE_RECURSE "${patched_root}")
        file(MAKE_DIRECTORY "${patched_root}")
        file(COPY "${source_root}/" DESTINATION "${patched_root}"
             PATTERN ".git" EXCLUDE)
        set(_target "${patched_root}/src/pio_usb.c")
        if(NOT EXISTS "${_target}")
            message(FATAL_ERROR "PIO-USB build copy failed: ${_target}")
        endif()
        file(READ "${_target}" _pio_c)
        # Exact known-old block from wiredopposite/Pico-PIO-USB. Fail closed
        # if upstream drifts; never silently emit firmware with the hang intact.
        set(_old [=[
  io_ro_32 *pc = &pp->pio_usb_tx->sm[pp->sm_tx].addr;
  while ((pp->pio_usb_tx->irq & IRQ_TX_ALL_MASK) == 0) {
    continue;
  }
  pp->pio_usb_tx->irq = IRQ_TX_ALL_MASK; // clear complete flag
  while (*pc < PIO_USB_TX_ENCODED_DATA_COMP) {
    continue;
  }
]=])
        set(_new [=[
  while ((pp->pio_usb_tx->irq & IRQ_TX_ALL_MASK) == 0) {
    continue;
  }
  pp->pio_usb_tx->irq = IRQ_TX_ALL_MASK; // clear complete flag

  // Upstream PR #206: the PIO SM only visits the EOP PC address for ~3
  // bit-times, then wraps. Polling PC can miss that fleeting interval and
  // permanently spin the host core. Wait the known EOP tail instead.
  // One USB bit = four TX state-machine clocks; works at FS and LS.
  const uint32_t tx_div = pp->low_speed ? pp->clk_div_ls_tx.div_int
                                         : pp->clk_div_fs_tx.div_int;
  busy_wait_at_least_cycles(16u * tx_div);
]=])
        string(FIND "${_pio_c}" "${_old}" _match)
        if(_match EQUAL -1)
            message(FATAL_ERROR
                "PIO-USB EOP wait does not match the tested upstream block. "
                "No patch applied; verify the library revision instead.")
        endif()
        string(REPLACE "${_old}" "${_new}" _pio_c "${_pio_c}")
        file(WRITE "${_target}" "${_pio_c}")
        file(WRITE "${_stamp_path}" "${_stamp}")
    endif()
    # Return the build-owned directory to the caller's scope before SDK init.
    set(PICO_PIO_USB_PATH "${patched_root}" PARENT_SCOPE)
endfunction()
