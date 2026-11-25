# NOTE (rumble fix, 2026-11-25): borealis 的 SwitchInputManager::clearVibration() 把
# 「频率」当成电机强度传给 sendRumbleInternal()，于是 amp 被写成 320（HidVibrationValue 的
# amp 合法区间是 0~1，1.0 即满幅）配上 ~0.78Hz 的次共振频率 —— 无线 Joy-Con 在这种驱动下
# 会「小幅持续震动」。它在控制器 style set 变化时被调用，正好命中「程序启动 + 手柄已取下」
# 的时机；按 HOME 出去再进来不发新指令就不震。子模块不直接提交改动，这里在配置期做一次
# 幂等替换（scripts/patch_borealis_rumble.py）；失败只告警，不阻断构建。
if (PLATFORM_SWITCH)
    set(_levelup_borealis_input
        "${CMAKE_CURRENT_SOURCE_DIR}/borealis/library/lib/platforms/switch/switch_input.cpp")
    if (EXISTS "${_levelup_borealis_input}")
        execute_process(
            COMMAND python3 "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/patch_borealis_rumble.py"
                    "${_levelup_borealis_input}"
            RESULT_VARIABLE _levelup_borealis_patch_rc
            OUTPUT_VARIABLE _levelup_borealis_patch_out
            ERROR_VARIABLE _levelup_borealis_patch_err)
        string(STRIP "${_levelup_borealis_patch_out}" _levelup_borealis_patch_out)
        message(STATUS "borealis clearVibration rumble fix: ${_levelup_borealis_patch_out}${_levelup_borealis_patch_err}")
    else ()
        message(WARNING "borealis clearVibration rumble fix skipped: switch_input.cpp not found")
    endif ()
endif ()

add_subdirectory(borealis/library)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_CURRENT_SOURCE_DIR}/cmake/")

set(BUILD_SHARED_LIBS OFF)

if (USE_MBEDTLS_CRYPTO)
    set(USE_MBEDTLS ON)
endif ()

add_subdirectory(moonlight-common-c)

if (NOT MULTICAST_DISABLED)
    set(MDNS_BUILD_EXAMPLE OFF)
    add_subdirectory(mdns)
endif ()
