# rmm_output_dir(<target> <dir>) - put a target's binaries in <dir> for every
# configuration (Visual Studio otherwise appends Debug/ or Release/).
function(rmm_output_dir target dir)
    set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${dir} LIBRARY_OUTPUT_DIRECTORY ${dir})
    foreach(cfg Debug Release RelWithDebInfo MinSizeRel)
        string(TOUPPER ${cfg} CFG)
        set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${CFG} ${dir} LIBRARY_OUTPUT_DIRECTORY_${CFG} ${dir})
    endforeach()
endfunction()

# rmm_add_plugin(<name> <sources...>)
#
# Builds an RMM plugin as a shared library named exactly <name>.dll / <name>.so
# (no "lib" prefix) into dist/rmm/plugins. Third-party plugin projects can
# copy this file and point `rmm_sdk` at RMM's include/ directory - plugins
# never link against RMM itself.
function(rmm_add_plugin name)
    add_library(${name} SHARED ${ARGN})
    target_link_libraries(${name} PRIVATE rmm_sdk)
    set_target_properties(${name} PROPERTIES PREFIX "")
    rmm_output_dir(${name} ${RMM_DIST}/plugins)
endfunction()
