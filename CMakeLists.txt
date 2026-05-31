cmake_minimum_required(VERSION 3.23)
project(creative_wakatime)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(include)

set(SOURCES
        main.cpp
        src/process_monitor.cpp
        src/file_watcher.cpp
        src/wakatime_client.cpp
        src/inbox_bridge.cpp
        src/tray_icon.cpp
        src/windows_dark_mode.cpp
        src/unity_focus_detector.cpp
)

set(HEADERS
        include/globals.h
        include/process_monitor.h
        include/file_watcher.h
        include/wakatime_client.h
        include/inbox_bridge.h
        include/tray_icon.h
        include/windows_dark_mode.h
        include/unity_focus_detector.h
)

add_executable(creative_wakatime ${SOURCES} ${HEADERS})

if(WIN32)
    target_compile_definitions(creative_wakatime PRIVATE
            WINVER=0x0601
            _WIN32_WINNT=0x0601
            UNICODE=1
            _UNICODE=1
            NOMINMAX
    )

    if(MINGW)
        target_link_options(creative_wakatime PRIVATE
                -static-libgcc
                -static-libstdc++
                -static
                -mwindows
                -Wl,--enable-stdcall-fixup
        )

        target_link_libraries(creative_wakatime
                winhttp
                shell32
                psapi
                ole32
                oleaut32
                windowscodecs
                dwmapi
                user32
        )
    else()
        # MSVC용 설정
        target_link_libraries(creative_wakatime
                winhttp
                shell32
                psapi
                ole32
                oleaut32
                windowscodecs
                dwmapi
                user32
        )
    endif()

    # Release 모드 설정 (중복 제거 및 정리)
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        if(MSVC)
            # MSVC: Windows 서브시스템 설정
            set_property(TARGET creative_wakatime PROPERTY WIN32_EXECUTABLE TRUE)
            target_link_options(creative_wakatime PRIVATE
                    /SUBSYSTEM:WINDOWS
                    /ENTRY:mainCRTStartup
            )
        elseif(MINGW)
            # MinGW: 크기 최적화 (-Os), LTO, 미사용 섹션 제거 (Resident Set Size 최소화)
            target_compile_options(creative_wakatime PRIVATE
                    -Os
                    -flto
                    -ffunction-sections
                    -fdata-sections
            )
            target_link_options(creative_wakatime PRIVATE
                    -Os  # LTO 링크 단계 최적화도 크기 우선으로
                    -s   # Strip symbols for smaller file size
                    -flto
                    -Wl,--gc-sections
            )
        endif()
    endif()
endif()

configure_file(${CMAKE_SOURCE_DIR}/logo_32.png ${CMAKE_BINARY_DIR}/logo_32.png COPYONLY)
