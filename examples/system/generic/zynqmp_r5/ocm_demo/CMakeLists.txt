collector_list  (_list PROJECT_INC_DIRS)
include_directories (${_list} ${CMAKE_CURRENT_SOURCE_DIR})

collector_list  (_list PROJECT_LIB_DIRS)
link_directories (${_list})

collector_list (_deps PROJECT_LIB_DEPS)
set (_src_common ${CMAKE_CURRENT_SOURCE_DIR}/init_${PROJECT_SYSTEM}.c)
set (_master_linker_script ${CMAKE_CURRENT_SOURCE_DIR}/master_lscript.ld)
set (_slave_linker_script ${CMAKE_CURRENT_SOURCE_DIR}/slave_lscript.ld)

set (_app_master master_ocm_demo)
set (_app_slave slave_ocm_demo)

set (_src_master ${CMAKE_CURRENT_SOURCE_DIR}/${_app_master}.c)
set (_src_slave ${CMAKE_CURRENT_SOURCE_DIR}/${_app_slave}.c)

list(APPEND _src_master ${CMAKE_CURRENT_SOURCE_DIR}/sys_init.c)
list(APPEND _src_slave  ${CMAKE_CURRENT_SOURCE_DIR}/sys_init.c)

list(APPEND _src_master ${CMAKE_CURRENT_SOURCE_DIR}/master_shmem_demod.c)
list(APPEND _src_slave  ${CMAKE_CURRENT_SOURCE_DIR}/slave_shmem_demod.c)

get_property (_linker_options GLOBAL PROPERTY TEST_LINKER_OPTIONS)
add_executable (${_app_master}.elf ${_src_master})
add_executable (${_app_slave}.elf ${_src_slave})

if (PROJECT_EC_FLAGS)
  string(REPLACE " " ";" _ec_flgs ${PROJECT_EC_FLAGS})
  target_compile_options (${_app_master}.elf PUBLIC ${_ec_flgs})
  target_compile_options (${_app_slave}.elf PUBLIC ${_ec_flgs})

endif (PROJECT_EC_FLAGS)
target_link_libraries(${_app_master}.elf -Wl,-Map=${_app_master}.map -Wl,--gc-sections -T\"${_master_linker_script}\" -Wl,--start-group ${_deps} -Wl,--end-group)
target_link_libraries(${_app_slave}.elf -Wl,-Map=${_app_slave}.map -Wl,--gc-sections -T\"${_slave_linker_script}\" -Wl,--start-group ${_deps} -Wl,--end-group)

install (TARGETS ${_app_master}.elf RUNTIME DESTINATION bin)
install (TARGETS ${_app_slave}.elf RUNTIME DESTINATION bin)



# vim: expandtab:ts=2:sw=2:smartindent
