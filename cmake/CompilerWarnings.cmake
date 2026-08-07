function(rfm_set_project_warnings target_name)
    if(MSVC)
        set(warnings /W4 /permissive-)
    else()
        set(
            warnings
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wsign-conversion
        )
    endif()

    if(RFM_WARNINGS_AS_ERRORS)
        if(MSVC)
            list(APPEND warnings /WX)
        else()
            list(APPEND warnings -Werror)
        endif()
    endif()

    target_compile_options(${target_name} PRIVATE ${warnings})
endfunction()

