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
    dc_ogx_fetch(btstack https://github.com/MegaCadeDev/btstack.git
        79c970b67d498c81f4ec4bb777a72da2531da8cb src/bluetooth.h)
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

function(dc_ogx_apply_patch source patch)
    execute_process(COMMAND "${GIT_EXECUTABLE}" apply --check --ignore-whitespace "${patch}"
                    WORKING_DIRECTORY "${source}" RESULT_VARIABLE _can_apply
                    OUTPUT_QUIET ERROR_QUIET)
    if(_can_apply EQUAL 0)
        execute_process(COMMAND "${GIT_EXECUTABLE}" apply --ignore-whitespace "${patch}"
                        WORKING_DIRECTORY "${source}" RESULT_VARIABLE _applied ERROR_VARIABLE _error)
        if(NOT _applied EQUAL 0)
            message(FATAL_ERROR "Cannot apply compatibility patch ${patch}: ${_error}")
        endif()
    else()
        execute_process(COMMAND "${GIT_EXECUTABLE}" apply --reverse --check --ignore-whitespace "${patch}"
                        WORKING_DIRECTORY "${source}" RESULT_VARIABLE _already_applied
                        OUTPUT_QUIET ERROR_QUIET)
        if(NOT _already_applied EQUAL 0)
            message(FATAL_ERROR "Compatibility patch does not match ${source}: ${patch}")
        endif()
    endif()
endfunction()

# The pinned forks already contain the OGX parser/L2CAP/report-count changes.
# Only BTstack's HIDS control-point scheduling still needs a local correction.
# Copy its used trees into an isolated build-owned working tree first.
if(DC_OGX_HAS_BLUETOOTH)
    set(DC_OGX_BLUEPAD32_SOURCE_DIR "${bluepad32_SOURCE_DIR}")
    set(DC_OGX_BTSTACK_SOURCE_DIR "${CMAKE_CURRENT_BINARY_DIR}/_deps/btstack-patched")
    file(MAKE_DIRECTORY "${DC_OGX_BTSTACK_SOURCE_DIR}")
    file(COPY "${btstack_SOURCE_DIR}/src" "${btstack_SOURCE_DIR}/platform"
              "${btstack_SOURCE_DIR}/3rd-party" "${btstack_SOURCE_DIR}/tool"
         DESTINATION "${DC_OGX_BTSTACK_SOURCE_DIR}")
    execute_process(COMMAND "${GIT_EXECUTABLE}" init --quiet "${DC_OGX_BTSTACK_SOURCE_DIR}" RESULT_VARIABLE _init_result)
    if(NOT _init_result EQUAL 0)
        message(FATAL_ERROR "Cannot initialize private dependency patch workspace: ${DC_OGX_BTSTACK_SOURCE_DIR}")
    endif()
    dc_ogx_apply_patch("${DC_OGX_BTSTACK_SOURCE_DIR}" "${CMAKE_CURRENT_LIST_DIR}/patches/btstack_hids_control_point.diff")
endif()
