# SightFlow VMS

RTSP 카메라 영상을 FFmpeg로 디코딩해 Qt/QML 화면에 실시간으로 표시하고, 화면
변화를 감지해 이벤트와 스냅샷을 기록·조회하는 C++ 영상관리시스템(VMS)
프로토타입입니다.

이 저장소는 단계별(phase-by-phase)로 개발되었습니다. 전체 개발 과정과 각
결정의 이유는 [`docs/DECISIONS.md`](docs/DECISIONS.md)에, 단계별 목표는
[`docs/ROADMAP.md`](docs/ROADMAP.md)에 기록되어 있습니다. 이 README는 "지금
저장소를 내려받은 사람이 실제로 무엇을 실행할 수 있는가"를 기준으로 작성했고,
구현되지 않은 기능은 적지 않았습니다.

## 구현된 기능

- **2채널 RTSP 라이브 영상 표시** (`sightflow-vms.exe`) — 고정된 두 채널
  `test`, `test2`를 동시에 디코딩·표시합니다. 연결 실패·중간 끊김 시 자동으로
  재연결을 시도하며, 재연결 전까지는 오래된 프레임을 화면에 남기지 않습니다.
- **독립 상태 서버** (`sightflow-server.exe`) — 같은 두 채널을 별도로
  디코딩하면서 MediaMTX 송출 상태, 디코딩 상태, 화면 변화 이벤트를 HTTP REST로
  제공합니다. 영상 표시용 클라이언트와 프로세스·스레드를 전혀 공유하지
  않습니다.
- **화면 변화 감지("화면 변화 감지" 이벤트)** — 디코딩된 프레임을 작은
  흑백 썸네일로 축소해 이전 프레임과 픽셀 단위로 비교하는 경량 알고리즘입니다.
  **사람/사물 인식이나 움직임 추적이 아니며, OpenCV나 AI 모델을 쓰지
  않습니다.** 일정 비율 이상 바뀐 화면이 일정 횟수 이상 반복될 때만 이벤트로
  확정됩니다.
- **이벤트 스냅샷** — 이벤트가 확정된 순간의 실제 디코딩 프레임을 FFmpeg의
  내장 MJPEG 인코더로 작게(긴 변 320px 이하) 인코딩해 보관합니다. 클라이언트
  화면에서 이벤트를 클릭하면 해당 스냅샷을 볼 수 있습니다.
- **SQLite 영속 저장** — 채널별 최근 이벤트(최대 20건)와 스냅샷을 로컬
  SQLite DB에 기록해 서버를 재시작해도 과거 기록을 다시 조회할 수 있습니다.
- **WebSocket 알림** — 새 이벤트가 기록되면 서버가 작은 알림(채널·ID 등, 이미지
  데이터 제외)을 WebSocket으로 즉시 보내 클라이언트가 해당 채널의 이벤트
  목록만 바로 다시 조회하게 합니다. WebSocket이 끊기거나 아예 쓸 수 없어도
  기존 2초 주기 REST 폴링이 계속 화면을 갱신합니다.

### 구현되지 않은 기능 (포트폴리오 열람 시 유의)

아래 기능은 이 저장소에 **포함되어 있지 않습니다.** VMS 분야에서 흔히 함께
언급되는 기능이지만, 혼동을 막기 위해 명시합니다.

- **ONVIF** 카메라 자동 탐색·제어 — 없음. 카메라 주소는 고정된 RTSP URL
  2개(`test`, `test2`)로 하드코딩되어 있습니다.
- **WebRTC** — 이 프로젝트의 코드는 WebRTC를 전혀 쓰지 않습니다. 시연에 쓰는
  MediaMTX 자체는 WebRTC 출력 기능이 있지만, 그것은 MediaMTX의 기능이지
  SightFlow VMS가 구현한 것이 아닙니다.
- **AI 기반 영상 분석(사람/사물/행동 인식)** — 없음. "화면 변화 감지"는 위에
  설명한 대로 픽셀 밝기 차이 비교일 뿐입니다.
- **영상 녹화·내보내기, 이벤트 검색/필터, 다중 사용자 인증** — 없음. HTTP/
  WebSocket API에는 인증이 전혀 없고(로컬호스트 전용 신뢰 모델), 이벤트는
  "최근 몇 건" 조회만 가능합니다.
- **범용 다채널 관리(채널 추가/삭제 UI)** — 없음. `test`/`test2` 두 채널이
  코드에 고정되어 있습니다.

## 구조

```
                         rtsp://127.0.0.1:8554/test, /test2
                                      │
                         ┌────────────┴────────────┐
                         │        MediaMTX          │  RTSP 서버 (저장소에 미포함)
                         │   Control API :9997       │
                         └────────────┬────────────┘
                 RTSP 직접 수신 (독립)  │  RTSP 직접 수신 (독립)
         ┌───────────────┴───────┐   └───────────────┐
         ▼                       ▼                   ▼
┌─────────────────────┐                  ┌─────────────────────────┐
│  sightflow-vms.exe   │                  │   sightflow-server.exe   │
│  (Qt/QML 클라이언트)  │   HTTP :8080     │  (상태/이벤트 서버)        │
│  - 채널별 디코딩 2개   │◄── REST 폴링 ────┤  - 채널별 DecodeWorker 2개│
│  - 2채널 동시 표시     │   (2초 주기)      │  - 화면 변화 감지          │
│  - 이벤트 목록/스냅샷  │  WS :8081        │  - 이벤트+스냅샷 SQLite   │
│    조회·표시           │◄── 변경 알림 ────┤    저장(채널당 최근 20건)  │
└─────────────────────┘  (있으면 즉시,     └─────────────────────────┘
                          없어도 폴링이 보강)
```

두 실행 파일은 MediaMTX에 각자 독립적으로 RTSP 연결을 맺습니다(영상 디코딩
파이프라인을 공유하지 않음). `sightflow-vms.exe`는 영상 표시 외에
`sightflow-server.exe`의 REST/WebSocket만 바라보며 상태·이벤트·스냅샷을
가져옵니다.

## 필수 준비물

| 항목 | 비고 |
|---|---|
| Windows 10/11 | 개발·검증 환경 |
| Visual Studio 2022, MSVC v143 툴셋 | "C++를 사용한 데스크톱 개발" + "C++ CMake 도구" 워크로드 |
| CMake ≥ 3.23 | VS2022에 포함된 CMake로 충분 |
| [vcpkg](https://github.com/microsoft/vcpkg) | FFmpeg(avcodec/avformat/swscale)를 받는 데 사용. 환경변수 `VCPKG_ROOT`가 로컬 vcpkg 경로를 가리켜야 함 |
| Qt 6.12 "msvc2022_64" 키트 | **Core, Gui, Qml, Quick, Network, Sql, WebSockets** 모듈 필요. 기본 설치에는 **Sql·WebSockets가 빠져 있을 수 있으니** Qt 유지관리 도구로 추가 설치 확인. 환경변수 `Qt6_DIR`이 `<키트 경로>\lib\cmake\Qt6`를 가리켜야 함 |
| [MediaMTX](https://github.com/bluenviron/mediamtx) | **저장소에 포함되어 있지 않습니다.** 받아서 저장소 루트의 `MediaMTX/` 폴더에 `mediamtx.exe`(+ 기본 `mediamtx.yml`)로 배치하세요. `.gitignore`로 제외되어 있어 저장소에는 올라가지 않습니다 |
| FFmpeg CLI(`ffmpeg.exe`) | **시연용 테스트 영상 송출에만 필요**(앱 빌드 자체는 vcpkg가 받는 FFmpeg 라이브러리만 씁니다). 저장소 루트의 `ffmpeg/bin/ffmpeg.exe`에 배치하세요. 역시 `.gitignore`로 제외됨 |

## VS2022 빌드 방법

1. "Developer PowerShell for VS 2022"(또는 x64 Native Tools 명령 프롬프트)를
   엽니다.
2. 환경변수를 설정합니다(세션마다 또는 영구적으로):
   ```powershell
   $env:VCPKG_ROOT = "C:\path\to\your\vcpkg"
   $env:Qt6_DIR = "C:\Qt\6.12.0\msvc2022_64\lib\cmake\Qt6"
   ```
3. 저장소 루트에서 구성 후 빌드합니다:
   ```powershell
   cmake --preset windows-vcpkg-vs2022
   cmake --build --preset windows-vcpkg-vs2022-debug
   ```
4. 결과물은 저장소 **바깥, 한 단계 위**의 형제 디렉터리에 생성됩니다
   (`CMakePresets.json`의 `binaryDir`이 `${sourceDir}/../...`로 설정되어
   있기 때문입니다):
   ```
   ..\sightflow-vms-build-vs2022-msvc143\Debug\sightflow-server.exe
   ..\sightflow-vms-build-vs2022-msvc143\Debug\sightflow-vms.exe
   ```
   두 실행 파일 모두 빌드 후 `windeployqt`가 Qt 런타임/플러그인을 같은
   폴더에 자동 배치합니다.

## 실행 순서 (요약)

자세한 단계별 시연 절차와 확인 포인트는
[`docs/DEMO.md`](docs/DEMO.md)를 참고하세요. 요약하면:

1. MediaMTX 실행
2. `test`, `test2` 두 채널에 테스트 RTSP 영상 송출 — 동봉된
   [`scripts/Start-TestStreams.ps1`](scripts/Start-TestStreams.ps1)로 자동화
   가능
3. `sightflow-server.exe` 실행
4. `sightflow-vms.exe` 실행 → 2채널 영상 확인
5. 한 채널의 테스트 영상을 중단·재시작 → 해당 패널만 "재연결 중..." 후 복구되는지 확인
6. 변화가 있는 채널의 이벤트 목록에 항목이 쌓이는지 확인 → 항목 클릭 →
   스냅샷 오버레이 확인
7. `sightflow-server.exe`를 종료 후 재실행 → 두 채널 모두 과거 이벤트가
   그대로 복원되는지 확인(콘솔에 `restored 20 persisted event(s)...` 로그)
8. 새 이벤트가 생길 때 폴링(최대 2초) 전에 거의 바로 목록이 갱신되는지
   관찰 → WebSocket 알림이 동작 중이라는 뜻

## API 주소

| 용도 | 주소 |
|---|---|
| RTSP (MediaMTX) | `rtsp://127.0.0.1:8554/test`, `rtsp://127.0.0.1:8554/test2` |
| MediaMTX Control API | `http://127.0.0.1:9997` |
| 채널 송출 상태 | `GET http://127.0.0.1:8080/channels/<test\|test2>` |
| 서버 디코딩 상태 | `GET http://127.0.0.1:8080/channels/<test\|test2>/metrics` |
| 최근 이벤트 목록 | `GET http://127.0.0.1:8080/channels/<test\|test2>/events` |
| 이벤트 스냅샷(JPEG) | `GET http://127.0.0.1:8080/channels/<test\|test2>/events/<id>/snapshot` |
| 이벤트 변경 알림(WebSocket) | `ws://127.0.0.1:8081/channels/<test\|test2>` |

모든 주소는 `127.0.0.1` 전용이며 인증이 없습니다(로컬 시연 목적).

SQLite 기록 위치: `%LOCALAPPDATA%\SightFlowVMS\events.sqlite3` (저장소나
빌드 폴더 밖, 사용자별 앱 데이터 경로). 삭제하면 다음 서버 실행 시 빈
기록으로 다시 시작합니다.

## 알려진 제한

- 채널이 `test`/`test2` 두 개로 고정되어 있습니다(코드에 하드코딩, 채널
  관리 UI 없음).
- "화면 변화 감지"는 사람/사물 인식이 아닌 단순 밝기 변화 비율 비교입니다.
  조명 변화나 카메라 자동노출 조정도 이벤트로 오인될 수 있습니다
  (자세한 임계값 근거는 `docs/DECISIONS.md`의 D22 참고).
- 채널당 이벤트/스냅샷 보관 개수는 최대 20건이며, 초과 시 가장 오래된
  기록(DB 포함)이 자동 삭제됩니다.
- HTTP/WebSocket API에 인증이 없습니다. 신뢰할 수 없는 네트워크에 그대로
  노출하지 마세요.
- 영상 녹화·내보내기, 이벤트 검색, 다중 사용자 기능은 없습니다.
- MediaMTX와 FFmpeg CLI는 저장소에 포함되어 있지 않으며 각자 받아 배치해야
  합니다.
- Windows + VS2022 환경에서만 빌드·검증되었습니다.

## 더 읽어보기

- [`docs/DEMO.md`](docs/DEMO.md) — 단계별 시연 절차
- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — 데이터 흐름, 스레드 모델,
  소유권 규칙
- [`docs/DECISIONS.md`](docs/DECISIONS.md) — 각 기능을 왜 그렇게 설계했는지
  (D1~D25)
- [`docs/ROADMAP.md`](docs/ROADMAP.md) — 단계별 개발 계획과 현재 범위
- [`docs/REQUIREMENTS.md`](docs/REQUIREMENTS.md) — 범위에 명시적으로
  포함/제외된 것들
