# ctest driver for tests/consumer: configure, build and run it with the
# compiler and flags of the crocket build under test. The h2o sources already
# fetched by that build are reused, so no network is needed.
foreach(step configure build run)
  if(step STREQUAL "configure")
    set(cmd ${CMAKE_COMMAND} -S ${SRC}/tests/consumer -B ${BIN} -G Ninja
        -DCROCKET_SOURCE_DIR=${SRC}
        -DFETCHCONTENT_SOURCE_DIR_H2O=${H2O_SRC}
        -DCMAKE_C_COMPILER=${CC} -DCMAKE_CXX_COMPILER=${CXX}
        -DCMAKE_C_COMPILER_LAUNCHER=${LAUNCHER} -DCMAKE_CXX_COMPILER_LAUNCHER=${LAUNCHER}
        -DCMAKE_EXE_LINKER_FLAGS=${LDFLAGS})
  elseif(step STREQUAL "build")
    set(cmd ${CMAKE_COMMAND} --build ${BIN})
  else()
    set(cmd ${BIN}/consumer)
  endif()
  execute_process(COMMAND ${cmd} RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "consumer ${step} failed (${rc})")
  endif()
endforeach()
