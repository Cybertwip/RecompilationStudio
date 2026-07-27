function(virtua_prepare_go_tool out_var module_root package_dir tool_name)
    if(tool_name STREQUAL "virtua"
       AND DEFINED VIRTUA_TOOL_EXECUTABLE
       AND NOT VIRTUA_TOOL_EXECUTABLE STREQUAL ""
       AND EXISTS "${VIRTUA_TOOL_EXECUTABLE}")
        set(${out_var} "${VIRTUA_TOOL_EXECUTABLE}" PARENT_SCOPE)
        return()
    endif()
    if(tool_name STREQUAL "apx"
       AND DEFINED POWER_APX_EXECUTABLE
       AND NOT POWER_APX_EXECUTABLE STREQUAL ""
       AND EXISTS "${POWER_APX_EXECUTABLE}")
        set(${out_var} "${POWER_APX_EXECUTABLE}" PARENT_SCOPE)
        return()
    endif()

    if(NOT DEFINED VIRTUA_GO_EXECUTABLE OR VIRTUA_GO_EXECUTABLE STREQUAL "")
        if(DEFINED POWER_GO_EXECUTABLE AND NOT POWER_GO_EXECUTABLE STREQUAL "")
            set(VIRTUA_GO_EXECUTABLE "${POWER_GO_EXECUTABLE}")
        elseif(DEFINED GO_EXECUTABLE AND NOT GO_EXECUTABLE STREQUAL "")
            set(VIRTUA_GO_EXECUTABLE "${GO_EXECUTABLE}")
        else()
            find_program(VIRTUA_GO_EXECUTABLE NAMES go REQUIRED)
        endif()
    endif()

    if(WIN32)
        set(_virtua_go_suffix ".exe")
    else()
        set(_virtua_go_suffix "")
    endif()

    set(_virtua_go_output "${CMAKE_CURRENT_BINARY_DIR}/${tool_name}${_virtua_go_suffix}")
    file(GLOB _virtua_go_sources CONFIGURE_DEPENDS "${package_dir}/*.go")

    add_custom_command(
        OUTPUT "${_virtua_go_output}"
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${CMAKE_CURRENT_BINARY_DIR}/go-cache"
            "${CMAKE_CURRENT_BINARY_DIR}/go-mod-cache"
            "${CMAKE_CURRENT_BINARY_DIR}/go-tmp"
        COMMAND ${CMAKE_COMMAND} -E env
            "GOTOOLCHAIN=local"
            "GOCACHE=${CMAKE_CURRENT_BINARY_DIR}/go-cache"
            "GOMODCACHE=${CMAKE_CURRENT_BINARY_DIR}/go-mod-cache"
            "GOTMPDIR=${CMAKE_CURRENT_BINARY_DIR}/go-tmp"
            "${VIRTUA_GO_EXECUTABLE}" build -buildvcs=false -trimpath -o "${_virtua_go_output}" .
        WORKING_DIRECTORY "${package_dir}"
        DEPENDS ${_virtua_go_sources} "${module_root}/go.mod"
        COMMENT "Building ${tool_name}${_virtua_go_suffix}"
        VERBATIM
    )

    set(${out_var} "${_virtua_go_output}" PARENT_SCOPE)
endfunction()
