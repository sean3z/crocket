# Acceptance criteria 1-4 and 6 (docs/DESIGN.md) and the production behavior: in-process, no sockets.
add_executable(crocket_acceptance acceptance.cpp)
target_link_libraries(crocket_acceptance PRIVATE crocket)
add_test(NAME acceptance COMMAND crocket_acceptance)

# JSON reading, writing, annotations and validation, plus problem+json bodies.
add_executable(crocket_json_test json.cpp)
target_link_libraries(crocket_json_test PRIVATE crocket)
add_test(NAME json COMMAND crocket_json_test)

# Async handlers: suspension, resumption on workers, context, callback<T>, sleep_for.
add_executable(crocket_async_test async.cpp)
target_link_libraries(crocket_async_test PRIVATE crocket)
add_test(NAME async COMMAND crocket_async_test)

# The trie router against the linear scan it replaced, on random route tables.
add_executable(crocket_router_test router.cpp)
target_link_libraries(crocket_router_test PRIVATE crocket)
target_include_directories(crocket_router_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
add_test(NAME router COMMAND crocket_router_test)

# The default number of event loops: physical cores from (fake) CPU topology.
add_executable(crocket_cpus_test cpus.cpp)
target_link_libraries(crocket_cpus_test PRIVATE crocket)
target_include_directories(crocket_cpus_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
add_test(NAME cpus COMMAND crocket_cpus_test)

add_executable(crocket_grpc_test grpc.cpp)
target_link_libraries(crocket_grpc_test PRIVATE crocket)
add_test(NAME grpc COMMAND crocket_grpc_test)

# Plain struct returns: JSON on HTTP routes, protobuf on gRPC routes; Json<T>/Proto<T> win.
add_executable(crocket_returns_test returns.cpp)
target_link_libraries(crocket_returns_test PRIVATE crocket)
add_test(NAME returns COMMAND crocket_returns_test)

# OpenAPI generated from the handlers, checked piece by piece, and served.
add_executable(crocket_openapi_test openapi.cpp)
target_link_libraries(crocket_openapi_test PRIVATE crocket)
add_test(NAME openapi COMMAND crocket_openapi_test)

# Fuzzing the parsers of untrusted input (tests/fuzz). Here each target runs its
# corpus and then a few seconds of mutations; ./dev fuzz runs them for longer,
# with coverage guidance and AddressSanitizer.
add_executable(crocket_fuzz fuzz/fuzz.cpp fuzz/targets.cpp fuzz/h2.cpp)
target_link_libraries(crocket_fuzz PRIVATE crocket)
if(CROCKET_HAS_AUTH)
  target_compile_definitions(crocket_fuzz PRIVATE CROCKET_HAS_AUTH=1)
  target_link_libraries(crocket_fuzz PRIVATE crocket::auth)
endif()
target_include_directories(crocket_fuzz PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(crocket_fuzz PRIVATE CROCKET_FUZZ_CORPUS="${CMAKE_CURRENT_SOURCE_DIR}/fuzz/corpus")
if(CROCKET_FUZZ_COVERAGE)
  target_compile_options(crocket_fuzz PRIVATE -fsanitize-coverage=trace-pc)
endif()
foreach(target json json_typed proto query route request h2)
  add_test(NAME fuzz.${target} COMMAND crocket_fuzz ${target} --seconds 2)
endforeach()
if(CROCKET_HAS_AUTH)
  add_test(NAME fuzz.jwt COMMAND crocket_fuzz jwt --seconds 2)
endif()

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
crocket_compile_fail(proto_duplicate_field "members 'size' and 'color' both have field number 2")
crocket_compile_fail(proto_unmapped_type "member 'grade' of proto message Widget has type char, which has no protobuf mapping")
crocket_compile_fail(rpc_two_messages "rpc bad::both: parameters 'a' and 'b' are both request messages")
crocket_compile_fail(bad_template "capture .\\{id. must be a whole segment of the form \\{name\\}")
crocket_compile_fail(json_name_clash
  "members 'user_id' and 'legacy_id' of Account both have the JSON name \"userId\"")
crocket_compile_fail(json_bad_pattern "invalid json::pattern \"\\^\\[A-Z\\]\\{3-\\}\\$\": expected '\\}' at offset 8")
crocket_compile_fail(log_arg_count
  "log message \"created user \\{user_id\\} on plan \\{plan\\}\": has 2 placeholders but 1 argument follow")
crocket_compile_fail(log_reserved_field "placeholder \\{request_id\\} is a field crocket writes itself")
crocket_compile_fail(json_check_type
  "json::min/max\\]\\] on member 'name' of Signup needs a number, but the member is std::")

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

if(CROCKET_HAS_AUTH)
  add_executable(crocket_auth_test auth.cpp)
  target_link_libraries(crocket_auth_test PRIVATE crocket::auth OpenSSL::Crypto OpenSSL::SSL)
  target_compile_options(crocket_auth_test PRIVATE -Wall -Wextra)
  add_test(NAME auth COMMAND crocket_auth_test)
  set_tests_properties(auth PROPERTIES TIMEOUT 90)
endif()

if(CROCKET_HAS_AUTH AND CROCKET_BUILD_EXAMPLES)
  add_test(NAME auth_not_in_core
           COMMAND ${CMAKE_COMMAND} -DBIN=$<TARGET_FILE:crocket_hello>
                   -P ${CMAKE_CURRENT_SOURCE_DIR}/auth_not_in_core.cmake)
endif()

# crocket::db against a real Postgres: CROCKET_TEST_DATABASE_URL, else skipped (exit 77).
if(CROCKET_HAS_PG)
  add_executable(crocket_db_test db.cpp)
  target_link_libraries(crocket_db_test PRIVATE crocket::pg)
  add_test(NAME db COMMAND crocket_db_test)
  set_tests_properties(db PROPERTIES SKIP_RETURN_CODE 77)
endif()

# Logging over real sockets: the line format byte for byte (tests/golden),
# redaction, no lost lines at moderate load, and order within a connection,
# with stderr to a file and to a pipe.
if(CURL AND CROCKET_BUILD_EXAMPLES)
  add_executable(crocket_log_server log_server.cpp)
  target_link_libraries(crocket_log_server PRIVATE crocket)
  add_test(NAME logging COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/logging.sh $<TARGET_FILE:crocket_log_server>
           ${CMAKE_CURRENT_SOURCE_DIR}/golden/log_format.txt)
  set_tests_properties(logging PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
endif()
