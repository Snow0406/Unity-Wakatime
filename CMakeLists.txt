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
)

set(HEADERS
        include/globals.h
        include/process_monitor.h
        include/file_watcher.h
        include/wakatime_client.h
        include/tray_icon.h
)

add_executable(unity_wakatime ${SOURCES} ${HEADERS})

if(WIN32)
    target_compile_definitions(unity_wakatime PRIVATE
            WINVER=0x0601
            _WIN32_WINNT=0x0601
            UNICODE=1
            _UNICODE=1
            NOMINMAX            # min/max 매크로 충돌 방지
    )

    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        set_property(TARGET unity_wakatime PROPERTY WIN32_EXECUTABLE TRUE)
        target_link_options(unity_wakatime PRIVATE
                /SUBSYSTEM:WINDOWS
                /ENTRY:mainCRTStartup
        )
    endif()

    target_link_libraries(unity_wakatime
            winhttp          # HTTP 클라이언트
            shell32          # 트레이 아이콘
            psapi            # 프로세스 정보
            wbemuuid         # WMI UUID 정의들
            ole32            # COM 기본 라이브러리
            oleaut32         # COM 자동화
            windowscodecs    # WIC PNG 로딩
            user32           # Windows UI API
    )

    if(MINGW)
        target_link_options(unity_wakatime PRIVATE
                -static-libgcc
                -static-libstdc++
                -Wl,--enable-stdcall-fixup
        )
    endif()
endif()

configure_file(${CMAKE_SOURCE_DIR}/logo_32.png ${CMAKE_BINARY_DIR}/logo_32.png COPYONLY)

# 릴리즈 빌드 시 추가 최적화
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    if(MINGW)
        target_link_options(unity_wakatime PRIVATE
                -s  # Strip symbols for smaller file size
        )
    endif()
endif()
