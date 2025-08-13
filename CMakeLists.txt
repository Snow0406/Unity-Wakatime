cmake_minimum_required(VERSION 3.23)
project(unity_wakatime)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 헤더 파일 디렉토리 추가
include_directories(include)

# 소스 파일들
set(SOURCES
        main.cpp
        src/file_watcher.cpp
        src/process_monitor.cpp
        src/wakatime_client.cpp
        src/tray_icon.cpp
)

# 헤더 파일들 (IDE에서 보이게 하기 위해)
set(HEADERS
        include/common.h
        include/file_watcher.h
        include/process_monitor.h
        include/wakatime_client.h
        include/tray_icon.h
)

add_executable(unity_wakatime ${SOURCES} ${HEADERS})

# Windows API 라이브러리들
if(WIN32)
    target_link_libraries(unity_wakatime
            winhttp
            shell32
            psapi
    )
endif()
