# crocket_hello links crocket::crocket only. The outbound client stays in
# crocket::client, so its error codes and metric names must not show up here.
execute_process(COMMAND strings ${BIN} OUTPUT_VARIABLE out RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "strings failed")
endif()
foreach(needle upstream.circuit_open crocket_client_requests_total crocket_client_breaker_state)
  if(out MATCHES "${needle}")
    message(FATAL_ERROR "crocket_hello contains \"${needle}\" from crocket::client")
  endif()
endforeach()
