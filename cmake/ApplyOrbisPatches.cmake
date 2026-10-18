find_package(Git REQUIRED)

function(apply_orbis_patch source_dir patch_file)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" apply --check "${patch_file}"
        WORKING_DIRECTORY "${source_dir}"
        RESULT_VARIABLE check_result
        OUTPUT_QUIET
        ERROR_VARIABLE check_error
    )

    if(check_result EQUAL 0)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" apply "${patch_file}"
            WORKING_DIRECTORY "${source_dir}"
            RESULT_VARIABLE apply_result
            ERROR_VARIABLE apply_error
        )
        if(NOT apply_result EQUAL 0)
            message(FATAL_ERROR "Failed applying ${patch_file}: ${apply_error}")
        endif()
        return()
    endif()

    execute_process(
        COMMAND "${GIT_EXECUTABLE}" apply --reverse --check "${patch_file}"
        WORKING_DIRECTORY "${source_dir}"
        RESULT_VARIABLE reverse_check_result
        OUTPUT_QUIET
        ERROR_QUIET
    )
    if(NOT reverse_check_result EQUAL 0)
        message(FATAL_ERROR
            "Cannot apply ${patch_file} to ${source_dir}. "
            "The pinned dependency source may have changed.")
    endif()

    message(STATUS "OpenOrbis patch already applied: ${patch_file}")
endfunction()
