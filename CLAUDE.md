# Claude Code Notes

## Build

- 검증 빌드는 실행(run)하지 말고 `scripts/build-all.bat`만 사용한다.
- 이 스크립트는 CLion bundled CMake와 Visual Studio BuildTools 환경으로 `Debug`, `Dev`, `Release`를 순서대로 빌드한다.
- Git Bash에서 실행할 때는 다음 명령을 사용한다.

```bash
cmd.exe //c '.\\scripts\\build-all.bat'
```

- 링크 단계에서 `creative_wakatime.exe`를 열 수 없다는 오류가 나면 기존 실행 파일이 실행 중인 상태다. 종료 여부를 사용자에게 확인한 뒤 다시 빌드한다.
