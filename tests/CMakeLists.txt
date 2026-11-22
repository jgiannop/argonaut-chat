find_package(Qt6 REQUIRED COMPONENTS Test)

# chat_test(<name> [LIBRARY <lib>...] [TIMEOUT <seconds>]): tests/tst_<name>.cpp
# as a CTest test (60 s unless TIMEOUT says). Every test runs headless, with its
# own HOME and XDG folders (TestSupport.h).
function(chat_test name)
    cmake_parse_arguments(ARG "" "TIMEOUT" "LIBRARY" ${ARGN})
    if(NOT ARG_LIBRARY)
        set(ARG_LIBRARY chat_core)
    endif()
    if(NOT ARG_TIMEOUT)
        set(ARG_TIMEOUT 60)
    endif()
    qt_add_executable(tst_${name} tst_${name}.cpp TestSupport.h)
    target_link_libraries(tst_${name} PRIVATE ${ARG_LIBRARY} Qt6::Test)
    add_test(NAME ${name} COMMAND tst_${name})
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT ${ARG_TIMEOUT})
endfunction()

chat_test(store)
chat_test(crypto)
# NIST's ML-KEM-1024 test vectors (some of ACVP's), for mlkem-native.
qt_add_resources(tst_crypto "acvp" PREFIX "/" FILES data/mlkem1024-acvp.json)
chat_test(ratchet)
chat_test(protocol)
chat_test(attachment)
chat_test(voice)
chat_test(history)
chat_test(server LIBRARY chat_server)
# Two devices through a real server, many cases: ~50 s here, near 60 on CI's Windows.
chat_test(messenger LIBRARY chat_server TIMEOUT 300)
chat_test(mainwindow LIBRARY chat_app chat_server)
chat_test(singleinstance LIBRARY chat_app)
