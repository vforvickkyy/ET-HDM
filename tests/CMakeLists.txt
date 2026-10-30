find_program(BASH_EXECUTABLE bash)

if(BASH_EXECUTABLE AND NOT WIN32)
  add_test(NAME engine
           COMMAND ${BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/engine/run_tests.sh
                   $<TARGET_FILE:ehfs-cli> ${CMAKE_CURRENT_BINARY_DIR}/engine-work)
  set_tests_properties(engine PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1800)
endif()
