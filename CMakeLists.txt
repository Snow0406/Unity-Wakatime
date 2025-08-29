cmake_minimum_required(VERSION 3.23)
project(unity_wakatime)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(include)

set(SOURCES
        main.cpp
        src/process_monitor.cpp
        src/file_watcher.cpp
        src/wakatime_client.cpp
        src/tray_icon.cpp
        src/unity_focus_detector.cpp
)

set(HEADERS
        include/globals.h
        include/process_monitor.h
        include/file_watcher.h
        include/wakatime_client.h
        include/tray_icon.h
        include/unity_focus_detector.h
)

add_executable(unity_wakatime ${SOURCES} ${HEADERS})

if(WIN32)
    target_compile_definitions(unity_wakatime PRIVATE
            WINVER=0x0601
            _WIN32_WINNT=0x0601
            UNICODE=1
            _UNICODE=1
            NOMINMAX
    )

    if(MINGW)
        target_link_options(unity_wakatime PRIVATE
                -static-libgcc
                -static-libstdc++
                -static
                -mwindows
                -Wl,--enable-stdcall-fixup
        )

        target_link_libraries(unity_wakatime
                winhttp
                shell32
                psapi
                wbemuuid
                ole32
                oleaut32
                windowscodecs
                user32
        )
    else()
        # MSVC용 설정
        target_link_libraries(unity_wakatime
                winhttp
                shell32
                psapi
                wbemuuid
                ole32
                oleaut32
                windowscodecs
                user32
        )
    endif()

    # Release 모드 설정 (중복 제거 및 정리)
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        if(MSVC)
            # MSVC: Windows 서브시스템 설정
            set_property(TARGET unity_wakatime PROPERTY WIN32_EXECUTABLE TRUE)
            target_link_options(unity_wakatime PRIVATE
                    /SUBSYSTEM:WINDOWS
                    /ENTRY:mainCRTStartup
            )
        elseif(MINGW)
            # MinGW: 심볼 스트리핑 (크기 최적화)
            target_link_options(unity_wakatime PRIVATE
                    -s  # Strip symbols for smaller file size
            )
        endif()
    endif()
endif()

configure_file(${CMAKE_SOURCE_DIR}/logo_32.png ${CMAKE_BINARY_DIR}/logo_32.png COPYONLY)
