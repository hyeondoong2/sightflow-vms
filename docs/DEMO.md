# 시연 절차

[`README.md`](../README.md)의 빌드 방법으로 `sightflow-server.exe`,
`sightflow-vms.exe`를 먼저 빌드해 두었다고 가정합니다. 아래 단계는 그 두
실행 파일이 저장소 바깥의 형제 디렉터리
`..\sightflow-vms-build-vs2022-msvc143\Debug\`에 있다고 가정합니다.

모든 경로는 저장소 루트 기준 상대 경로로 적었습니다. 이 문서나
`scripts/` 안의 스크립트에는 특정 PC의 절대경로가 들어 있지 않습니다.

## 0. 준비: MediaMTX + 테스트 영상 송출

`MediaMTX/mediamtx.exe`와 `ffmpeg/bin/ffmpeg.exe`를 README의 안내대로
미리 받아 배치해 두세요.

저장소 루트에서 PowerShell로:

```powershell
.\scripts\Start-TestStreams.ps1
```

이 스크립트는 MediaMTX를 띄우고, `test`/`test2` 채널에 가벼운 합성
영상(`testsrc`, 320x240, 10fps — 무거운 `mandelbrot` 등은 쓰지 않습니다)을
RTSP로 송출하기 시작합니다. 콘솔에 각 프로세스의 PID와 로그 파일 위치가
출력됩니다. 창을 닫지 말고 그대로 두면 됩니다(백그라운드로 실행되고,
스크립트 자체는 바로 종료됩니다).

확인:
```powershell
curl.exe http://127.0.0.1:9997/v3/paths/list
```
`test`, `test2` 두 경로가 `"ready": true`로 보이면 정상입니다.

## 1. 서버 실행

새 PowerShell 창에서:

```powershell
..\sightflow-vms-build-vs2022-msvc143\Debug\sightflow-server.exe
```

콘솔에 다음과 비슷한 줄이 보여야 합니다. (`restored ... persisted event(s)`
줄은 해당 채널에 이전에 저장된 이벤트가 있을 때만 찍힙니다 — 아직 SQLite
기록이 없는 첫 실행이라면 이 줄 없이 바로 `listening on ...` 줄만
보이는 게 정상입니다.)
```
sightflow-server: restored 20 persisted event(s) for 'test'
sightflow-server: restored 20 persisted event(s) for 'test2'
sightflow-server: listening on http://127.0.0.1:8080 (MediaMTX API at http://127.0.0.1:9997), WebSocket notifications on ws://127.0.0.1:8081
```

이 창도 닫지 말고 그대로 둡니다 — 나중에 "서버 재시작" 시연에서 다시 씁니다.

## 2. 클라이언트 실행 → 2채널 영상 확인

새 PowerShell 창에서:

```powershell
..\sightflow-vms-build-vs2022-msvc143\Debug\sightflow-vms.exe
```

**확인할 것:** 창이 좌/우 두 패널로 나뉘고, `test`/`test2` 영상이 각각
재생됩니다. 각 패널 위에 "MediaMTX 송출 있음 · 서버 디코딩 running" 같은
상태 줄이 보입니다.

## 3. 채널 하나 중단·재연결

`scripts/Start-TestStreams.ps1`을 실행했던 창(또는 새 창)에서 `test2`
영상만 멈췄다가 다시 띄웁니다:

```powershell
.\scripts\Stop-TestStreams.ps1 -Channel test2
# 잠시 기다린 뒤
.\scripts\Start-TestStreams.ps1 -Channel test2
```

**확인할 것:**
- 멈추는 즉시 `sightflow-vms.exe`의 **test2 패널만** "재연결 중..."으로
  바뀌고 영상이 멈춥니다. **test 패널은 전혀 영향받지 않습니다.**
- 다시 띄우면 몇 초 안에 test2도 자동으로 복구됩니다(재연결을 수동으로
  누를 필요 없음).

## 4. 화면 변화 이벤트 확인

두 채널 모두 합성 패턴이 계속 움직이므로, 별다른 조작 없이 수 초 간격으로
각 패널의 "화면 변화 감지" 목록에 새 항목(시각, 변화 비율, `[스냅샷]`
표시)이 쌓이는 것을 볼 수 있습니다.

**확인할 것:** 두 채널의 이벤트 번호(ID)가 서로 다른 독립된 순서로
증가합니다(한쪽 채널의 이벤트가 다른 채널 목록에 섞이지 않음).

## 5. 스냅샷 클릭

`[스냅샷]`이 붙은 이벤트 중 하나를 클릭합니다.

**확인할 것:** 해당 패널 위에 그 순간 실제로 디코딩됐던 정지 이미지가
오버레이로 뜹니다(합성 영상이므로 그 순간의 패턴이 그대로 보임). 우측 상단
"✕"로 닫으면 다시 실시간 영상으로 돌아갑니다.

## 6. 서버 재시작 → SQLite 기록 복원

1단계에서 띄워 둔 `sightflow-server.exe` 창에서 `Ctrl+C`로 종료합니다.
`sightflow-vms.exe`의 두 패널 상태 줄이 "서버 연결 안 됨"으로 바뀌는지
확인합니다(영상 자체는 MediaMTX에 직접 연결되어 있으므로 계속 재생됩니다).

같은 포트로 다시 실행합니다:

```powershell
..\sightflow-vms-build-vs2022-msvc143\Debug\sightflow-server.exe
```

**확인할 것:**
- 콘솔에 `sightflow-server: restored 20 persisted event(s) for 'test'`
  (그리고 `test2`도 동일)처럼, 재시작 전까지 쌓였던 이벤트 수가 그대로
  복원됐다는 로그가 보입니다.
- `sightflow-vms.exe`는 자동으로 다시 연결되고(수동 재시작 불필요), 4~5
  단계에서 보던 과거 이벤트와 스냅샷을 그대로 다시 클릭해 볼 수 있습니다.
  이벤트 시각이 "MM/dd hh:mm:ss" 형식으로 날짜까지 표시되어, 재시작 전
  기록임을 "방금 생긴 일"과 구분할 수 있습니다.

## 7. WebSocket 알림 체감

6단계를 마친 상태에서 두 패널을 계속 지켜보세요.

**확인할 것:** 새 이벤트가 생길 때 화면 목록이 거의 즉시(기존 2초 주기
폴링을 기다리지 않고) 갱신되는 것이 보통입니다 — 이것이 WebSocket 알림이
동작하고 있다는 뜻입니다. 네트워크 상황에 따라 WebSocket이 일시적으로
끊겨도 **최대 2초 안에는 기존 REST 폴링이 같은 결과로 화면을 맞춰줍니다** —
WebSocket 유무와 무관하게 화면이 틀어지지 않는다는 것이 이 설계의 핵심이며,
WebSocket은 어디까지나 "더 빠르게 알려주는" 보조 수단입니다.

**"화면이 빨라 보인다"는 인상이 아니라 실제로 증명하려면**, 별도 터미널에서
curl의 실험적 WebSocket 지원으로 서버에 직접 붙어 원시 메시지를 받아보세요
(curl 8.x 필요):

```powershell
curl.exe -N ws://127.0.0.1:8081/channels/test
```

새 이벤트가 생길 때마다 `{"changeRatio":...,"channel":"test","id":<N>,
"snapshotAvailable":true}` 형태의 JSON이 그대로 찍힙니다. 이 `id`와
`changeRatio` 값을 같은 시간대의 `curl.exe http://127.0.0.1:8080/channels/test/events`
결과와 비교하면, WebSocket으로 받은 값이 서버에 실제로 기록된 이벤트와
정확히 일치함을 눈으로 확인할 수 있습니다 — "빠르게 갱신되는 것 같다"는
느낌이 아니라, 같은 id/changeRatio가 두 경로 모두에서 나온다는 것이
객관적 증거입니다.

## 8. 정리

```powershell
.\scripts\Stop-TestStreams.ps1
```

이 스크립트는 `Start-TestStreams.ps1`이 **직접 띄운 프로세스만** 종료합니다
(PID를 저장해 두었다가 그 PID가 여전히 mediamtx.exe/ffmpeg.exe인지 확인한
뒤에만 종료). 다른 이유로 떠 있는 mediamtx.exe/ffmpeg.exe는 건드리지
않습니다. `sightflow-server.exe`/`sightflow-vms.exe`는 각 콘솔 창에서
`Ctrl+C`(서버) 또는 창 닫기(클라이언트)로 직접 종료하세요.
