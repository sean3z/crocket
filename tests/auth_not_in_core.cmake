# crocket_hello links crocket::crocket only. JWT fetch and error strings stay in
# crocket::auth, so they must not show up in that binary.
execute_process(COMMAND strings ${BIN} OUTPUT_VARIABLE out RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "strings failed")
endif()
foreach(needle openid-configuration auth.unknown_key jwks_uri)
  if(out MATCHES "${needle}")
    message(FATAL_ERROR "crocket_hello contains \"${needle}\" from crocket::auth")
  endif()
endforeach()
