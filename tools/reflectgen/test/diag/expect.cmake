# ctest: reflectgen must reject every misuse in Bad.h, each with its own message.
execute_process(COMMAND ${REFLECTGEN} ${HEADER} -o ${OUT} -- ${ARGS}
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
if(rc EQUAL 0)
    message(FATAL_ERROR "reflectgen accepted Bad.h:\n${out}")
endif()
foreach(expected
        "unknown REFLECT key 'bogus'"
        "slider on field 'a' needs both min and max"
        "unknown PROP key 'colour'"
        "setter = is for PROP on a getter method, not on field 'c'"
        "field 'd' is not public; add REFLECT_FRIEND to A"
        "REFLECT on a class template is not supported"
        "field 'secret' is private to its class, so PDerived cannot register it"
        "setter 'set' is overloaded"
        "PROP on a method needs a non-static const getter")
    string(FIND "${out}" "${expected}" pos)
    if(pos EQUAL -1)
        message(FATAL_ERROR "missing diagnostic: ${expected}\n--- reflectgen said:\n${out}")
    endif()
endforeach()
message(STATUS "PASS: every misuse reported")
