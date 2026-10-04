# Acceptance 1-4, 6 and the production behaviour: in-process, no sockets.
add_executable(crocket_acceptance acceptance.cpp)
target_link_libraries(crocket_acceptance PRIVATE crocket)
add_test(NAME acceptance COMMAND crocket_acceptance)

add_executable(crocket_grpc_test grpc.cpp)
target_link_libraries(crocket_grpc_test PRIVATE crocket)
add_test(NAME grpc COMMAND crocket_grpc_test)

# Acceptance 5 and friends: programs that must NOT compile. Each is an
# EXCLUDE_FROM_ALL object library; the test builds it and expects failure with
# a diagnostic matching the regex (both identifiers for the capture mismatch).
function(crocket_compile_fail name regex)
  add_library(cf_${name} OBJECT EXCLUDE_FROM_ALL compile_fail/${name}.cpp)
  target_link_libraries(cf_${name} PRIVATE crocket)
  add_test(NAME compile_fail.${name}
           COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target cf_${name})
  set_tests_properties(compile_fail.${name} PROPERTIES PASS_REGULAR_EXPRESSION "${regex}")
endfunction()

crocket_compile_fail(capture_name_mismatch
  "path capture '\\{age\\}' has no parameter named 'age'; did you mean parameter 'years'")
crocket_compile_fail(capture_type_unparsable "parameter 'b' binds to path capture '\\{b\\}'.*Blob")
crocket_compile_fail(unknown_extractor "parameter 'w' of type 'Widget' is not a path capture and not an extractor")
crocket_compile_fail(bad_return_type "NotAResponse")
crocket_compile_fail(from_path_not_optional
  "json::from_path\\]\\] member 'id' of Widget must be std::optional<P>")
crocket_compile_fail(bad_header_name "not a valid HTTP header name")
crocket_compile_fail(proto_duplicate_field "members 'size' and 'colour' both have field number 2")
crocket_compile_fail(proto_unmapped_type "member 'grade' of proto message Widget has type char, which has no protobuf mapping")
crocket_compile_fail(rpc_two_messages "rpc bad::both: parameters 'a' and 'b' are both request messages")
crocket_compile_fail(bad_template "capture .\\{id. must be a whole segment of the form \\{name\\}")

# Control: the valid twin must compile (guards against the harness "passing"
# because everything fails to build).
add_library(cf_control_ok OBJECT EXCLUDE_FROM_ALL compile_fail/control_ok.cpp)
target_link_libraries(cf_control_ok PRIVATE crocket)
add_test(NAME compile_fail.control_ok COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target cf_control_ok)

# Acceptance 7: real sockets, TLS, ALPN h2 (needs openssl + curl with HTTP/2).
find_program(CURL curl)
find_program(OPENSSL openssl)
if(CURL AND OPENSSL AND CROCKET_BUILD_EXAMPLES)
  add_test(NAME tls_h2 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tls_h2.sh $<TARGET_FILE:crocket_serve>
           $<TARGET_FILE:crocket_hello>)
  set_tests_properties(tls_h2 PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
endif()

# gRPC over real sockets: h2 prior knowledge and TLS, driven by curl.
if(CURL AND OPENSSL AND CROCKET_BUILD_EXAMPLES)
  add_test(NAME grpc_h2 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/grpc_h2.sh $<TARGET_FILE:crocket_grpc>)
  set_tests_properties(grpc_h2 PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
endif()

# An application that pulls crocket in with FetchContent builds and runs.
add_test(NAME consumer
         COMMAND ${CMAKE_COMMAND} -DSRC=${PROJECT_SOURCE_DIR} -DBIN=${CMAKE_CURRENT_BINARY_DIR}/consumer
                 -DH2O_SRC=${h2o_SOURCE_DIR} -DCC=${CMAKE_C_COMPILER} -DCXX=${CMAKE_CXX_COMPILER}
                 "-DLAUNCHER=${CMAKE_CXX_COMPILER_LAUNCHER}" "-DLDFLAGS=${CMAKE_EXE_LINKER_FLAGS}"
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/consumer.cmake)
set_tests_properties(consumer PROPERTIES TIMEOUT 600)
