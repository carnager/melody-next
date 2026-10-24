# SPDX-License-Identifier: GPL-3.0-only

# Every test and benchmark of the calling directory runs apart from the user
# who runs it: data and cache directories of its own, a runtime directory of
# its own -- so no engine socket of theirs is found, nor theirs of it -- its
# own melodyd.conf and discovery service, and no systemd. A UI benchmark run
# without this once moved a user's library out from under their engine and
# handed their engine its test settings (ADR-0287).
#
# The config directory stays the real one: fontconfig reads it, and the
# layout tests measure rows in the desktop's font; settings are isolated by
# the tests themselves. Audio is still the machine's: PipeWire is found where
# it was.
#
# The desktop's Qt platform theme is not loaded. Plasma's brings
# KNotifications, whose event sounds (libcanberra over PipeWire's ALSA plugin)
# deadlock when torn down at exit: every test of the window passed and its
# process never ended, so ctest never returned.
function(melody_isolate_tests)
    get_property(tests DIRECTORY PROPERTY TESTS)
    if(NOT tests)
        return()
    endif()
    foreach(test IN LISTS tests)
        set(home "${CMAKE_CURRENT_BINARY_DIR}/test-home/${test}")
        set_property(TEST ${test} APPEND PROPERTY ENVIRONMENT
            "XDG_DATA_HOME=${home}/data"
            "XDG_CACHE_HOME=${home}/cache"
            # And state: melody-together keeps its rooms there.
            "XDG_STATE_HOME=${home}/state"
            # Plugins of its own: on macOS the data folders are not XDG's.
            "MELODY_PLUGIN_PATH=${home}/plugins"
            # Engines a test starts are announced under a service of its own:
            # not found by the real ones on the network, nor by other tests.
            "MELODY_DISCOVERY_SERVICE=_mt-${test}._tcp")
    endforeach()

    # The engines tests start leave their socket locks there, one per state
    # directory. Owner-only, as a runtime directory has to be.
    set(runtime "${CMAKE_BINARY_DIR}/test-runtime")
    file(MAKE_DIRECTORY "${runtime}")
    file(CHMOD "${runtime}" DIRECTORY_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
    set(environment "XDG_RUNTIME_DIR=set:${runtime}" "QT_QPA_PLATFORMTHEME=unset:")
    if(DEFINED ENV{PIPEWIRE_RUNTIME_DIR})
        list(APPEND environment "PIPEWIRE_RUNTIME_DIR=set:$ENV{PIPEWIRE_RUNTIME_DIR}")
    elseif(DEFINED ENV{XDG_RUNTIME_DIR})
        list(APPEND environment "PIPEWIRE_RUNTIME_DIR=set:$ENV{XDG_RUNTIME_DIR}")
    endif()
    set_tests_properties(${tests} PROPERTIES ENVIRONMENT_MODIFICATION "${environment}")

    # ADR-0286: no test reads or writes the melodyd.conf of whoever runs it,
    # and no test engine plays for the engines on the network unless asked.
    set(config "${CMAKE_CURRENT_BINARY_DIR}/test-melodyd.conf")
    file(WRITE "${config}" "# Every test engine's, unless it names its own.\nagent = no\n")
    set_property(TEST ${tests} APPEND PROPERTY ENVIRONMENT "MELODYD_CONFIG=${config}"
                                                          "MELODY_NO_SYSTEMD=1")
endfunction()
