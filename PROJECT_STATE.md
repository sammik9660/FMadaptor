# PROJECT_STATE

최종 기록일: 2026-10-02. 프로젝트 표시 이름: **Samsung USB-C FM Radio Adapter**.
현재 단계: hardware-verified prototype를 main에 통합·push 완료. 이번 작업은 문서 동기화만 수행한다.

README는 외부 소개, 이 파일은 후속 개발 세션의 상태 인계, AGENTS.md는 작업 규칙이다. 기록과 실제 Git 상태를 함께 확인한다. CONFIRMED는 코드/빌드/사용자 관찰의 근거를 구분하며 UNVERIFIED와 구현되지 않은 기능을 완료로 승격하지 않는다.

## Current Goal

Samsung built-in FM Radio app/service를 controller로 사용하여 RP2040 + SI4703 외부 튜너의 FM 기능을 제공하는 Adapter를 유지보수한다. 현재 검증된 legacy firmware 보존이 기능 개선보다 우선이다. 별도의 Android FM 앱이나 정품 이어폰 전체 기능 복제 프로젝트로 설명하지 않는다.

## Canonical Source and Preservation

- 현재 canonical firmware: `firmware/fm_adapter/fm_adapter.ino`.
- 원본 archive: 기존 `어댑터` 폴더의 31개 자료. 수정·이동·삭제·이름 변경하지 않는다.
- 최초 canonical TXT: `1. 최종코드에 주석 추가 버전.txt`. 실제 최종 동작 TXT와 주석/공백 외 코드가 동일하다는 사용자 확인을 받았다.
- 최초 TXT→INO 복사는 byte-for-byte 동일했고 SHA-256은 `407CC1A16C573DD90F49A0C0F213EEBBE4428F009DC69F02C21BC156A62088B8`이었다. 이는 **최초 baseline**의 해시이며 현재 firmware 해시가 아니다.
- 최초 baseline commit: `bb134825cd34ffde106fb25d4aec5ccec7e5c8f7`.
- 직접 기록 복구: `ee34972b1e0597dc0ff8247298f3ee42c1fea4db`, `historical/recovered-known-good`. 당시 전체 소스 출력과 실제 패치를 재생했다. 원래 바이너리/인코딩/EOL 동일성은 확정하지 않는다. 상세 근거는 [recovered-known-good.md](docs/protocol/recovered-known-good.md)에 보존한다.
- hardware-verified commit: `a15fce261708a2150e12d6dc011ce3794606c2b7`, `experiment/si470x-setup-sda`.
- main 통합: `c94294bc829e44bb5e68e271b6d399e4fa02be47`. 일반 merge, 두 이력 보존. firmware는 a15fce2와 바이트 동일. 실험 branch도 로컬/원격에 보존한다.
- 현재 검증 firmware SHA-256: `7A6C20043EF494FD0DD74BC7387234CE4A4A6D17285A437EFD672BD5852CCD7C`.

## Confirmed Physical Results

**CONFIRMED — 사용자 실기 확인**

RP2040-Zero + SI4703, SDA GPIO4 / SCL GPIO5 / RESET GPIO2에서 Samsung FM 앱의 normal earphone mode, FM playback/tuning이 정상 동작했다. 이전의 심한 tuning 지연과 continuous loud beep가 해당 시험에서는 없었다. 사용자는 volume/mute control과 SEEK command support도 현재 기능으로 확인했다. SEEK 지원과 방송국 판정의 신뢰성은 구분한다.

별도 최소 진단 sketch에서 GPIO8/9는 반복적으로 address ACK를 얻지 못했고, 물리적으로 GPIO4/5로 옮긴 뒤 완전 전원 재인가를 반복해도 0x10 ACK 및 2-byte read가 성공했다. 이는 관찰이며 기존 배선 실패 원인을 확정하지 않는다.

정확한 휴대폰 모델/Android/FM 앱 버전, 시험 횟수와 전체 전원 조건은 아직 미기록이다. 이 세션에서 새로운 실기 시험을 수행한 것은 아니다.

## Confirmed Implementation and Decisions

**CONFIRMED — 현재 코드 및 설치 라이브러리 대조**

- Arduino `setup()` / `loop()`, TinyUSB 및 `SI470X rx` 사용.
- RESET=2, SDA=4, SCL=5. 100 kHz I2C. `DUMMY_INT=4` 정의는 남아 있지만 사용되지 않는다.
- 초기화 순서: `Wire.setSDA` → `Wire.setSCL` → `Wire.setClock(100000)` → `rx.setup(RESET_PIN, SDA_PIN)`. 선행 `Wire.begin()` 없음.
- PU2CLR setup 두 번째 인자는 SDA이다. SDA OUTPUT LOW → RESET → 내부 Wire.begin() → powerUp() 순서. Arduino-Pico 6.1.1 begin은 이미 실행 중이면 반환하므로 선행 begin을 다시 추가하지 않는다.
- VID 0x04E8 / PID 0xA05B. BesCmd SET=161 / GET=162 / QUERY=163.
- SET 0/4/5: mute 기반 radio on/off, mute, volume. CMD7 SEEK / CMD9 direct tune은 callback에서 `pending_cmd` / `target_val` 예약 후 main loop에서 실행.
- GET8 저장 볼륨 / GET13 저장 주파수 / GET17=0. 기타 GET 기본값 1. GET2/GET18/SET14에 별도 기능 구현을 주장하지 않는다.
- QUERY: success flag, 고정 byte 1, frequency low/high, RSSI. 저장값을 반환하며 새 측정을 시작하지 않는다.
- `setBand(0)`, `setSpace(0)`: 설치 라이브러리에서 87.5–108 MHz / 200 kHz. 초기 current_freq=10770, current_vol=7, setup mute=true/volume=0.
- legacy `safeTune()` → `rx.setChannel()` 유지. CMD9 후 RSSI 조회, success=true, notify_state=1. bounded tune/recovery/watchdog 없음.
- SEEK는 `rx.seek(0, direction)` 반환 후 60ms 초과 조건에서 주파수/RSSI 처리. station success는 `last_rssi > 15`.
- Endpoint 0x85 notify: `01 00 08 00 00`, `01 01 <freq low> <freq high> <RSSI>`. Busy 검사와 CMD9 transfer return 기록은 존재하나 실패 결과에 따른 전용 retry는 없다.
- DebugSnapshot v1 / packed 88 bytes / 4 slots / 13 stage flags/timestamps. EP0 read: C0/D9, value 464D, index 1, length 88. 최신 incomplete 우선 조회.

Callback/main-loop 분리를 근거 없이 되돌리지 않는다. Control ACK는 tune 완료 증명이 아니다. SET 0/4/5는 callback에서 tuner 접근하며 라이브러리 tune/seek polling은 blocking될 수 있으므로 전체 firmware가 완전히 non-blocking이라고 주장하지 않는다. 단일 pending 슬롯 및 concurrency, 입력/전송 검증은 향후 검토 대상이다.

## Frequency Handling

기본 표현 `10770 = 107.70 MHz`, 정수 1은 10 kHz다. safeTune은 freq_val>5000이면 `(freq_val - 8750) / 20`, 그 외 `(freq_val - 875) / 2`로 채널을 계산하여 setChannel에 전달한다.

SEEK는 `fake_f=rx.getFrequency()`, `channel=(fake_f-8750)/10`, `real_f=8750+channel*20`으로 보정한다. 기존 동작 수식을 보존한다. 모든 library 버전의 결함으로 일반화하지 않는다. Direct Tune 저장 주파수는 요청값이며 실측 재확인값이 아니다.

## Build Environment

확인된 reference: Earle Philhower Arduino-Pico 6.1.1, Waveshare RP2040 Zero, Adafruit TinyUSB stack / library 3.7.7, PU2CLR SI470X 1.0.5, `usbstack=tinyusb`. GPIO4/5 build 성공: flash 85,788 bytes / global RAM 17,836 bytes. 외부 library는 repository에 vendoring하지 않는다.

설치 PU2CLR source 경로는 Arduino user libraries의 `PU2CLR_SI470X/src`다. 확인 해시:

- SI470X.cpp: `FB4E1FCFF57D0EC7A695E604EF51CF332D9F8927D9AE076B5B9059B682AF46E7`
- SI470X.h: `7DC902611DAC191B1BA44EF2DB00BCCFBDA26586D851333B358FD307592FF7CE`

PC의 전역 core 6.2.0과 격리 6.1.1 build를 구분한다. 과거 성공 flash의 binary hash/업로드 로그 대응은 미확정이다. 당시 기록에서도 같은 source의 실패와 재연결 후 성공이 공존했다.

main의 `diagnostics/android-fm-debug`는 후대 v4 parser다. 현재 v1/88-byte firmware와 호환된다고 안내하지 않는다. v1 reader는 `historical/working-snapshot-v1`에 남아 있다. 이번 문서 작업은 diagnostic code를 수정/제거하지 않는다.

## Known Issues / Unverified / Out of Scope

- SEEK/RSSI threshold의 오검출과 이후 탐색 진행 문제: 사용자 보고. reliable station detection 미완료.
- Samsung earphone/speaker mode UI가 원하는 시점에 항상 나타나지 않음. normal earphone mode 자체는 확인됨.
- abnormal initialization/connection에서 loud beep/pop 사례. GPIO4/5 정상 시험 결과를 모든 오류/전원 상태의 무발생 보장으로 확대하지 않음.
- standalone UI, OLED, physical controls: future ideas.
- PCM over USB, phone speaker/Bluetooth routing, full USB Audio: 구현 완료 기능 아님.
- 완전 descriptor 복제, 인증 우회, 범용 Samsung 호환성, bug-free/production-ready 상태를 주장하지 않음.
- 세부 전원/회로/모듈 모델, phone/OS/app 버전, 장기 신뢰성 미확인.
- 과거 timeout은 약 20초/30초 표현이 혼재. 정확한 고정 측정값으로 기록하지 않음.

## Do Not Break

검증 commit을 보존한 뒤 사용자 요청 범위 안에서만 변경한다. 자동 refactor/format, 원본 archive 변경, 근거 없는 frequency/USB 구조 변경을 금지한다. 문서 작업에서도 source hash/diff를 검증한다. README의 성공 표현은 코드 존재와 실기 결과를 구분한다.

## Migration Status

- [x] Original inventory / canonical identification / initial unmodified copy and integrity verification
- [x] README / PROJECT_STATE / AGENTS / .gitignore / minimal docs
- [x] Initial public-repository security preflight / MIT license decision
- [x] Git initialization / original baseline commit / origin connection / first push
- [x] Historical source recovery and evidence preservation
- [x] SDA setup argument / initialization-order / GPIO4/5 experiments committed
- [x] User physical verification / verified experiment pushed / main merged and pushed
- [ ] Original presentation/photos/raw log documentation import — existing recovery evidence is separate
- [ ] SEEK validation / playback-mode / recovery / transient improvements
- [ ] Diagnostic removal / architectural cleanup / optional standalone/OLED controls

Security checks apply to the reviewed files at their recorded time, not future imports. Original photos/logs remain outside the repository. Review identifying information before imports. The project's own code is MIT, Copyright (c) 2026 sammik9660; external dependency licenses remain separate. Prior work acknowledgement is not proof of code copying or a license relationship.

## Next Action

Await a separately scoped user request. Repository rename/description changes are proposals only; code improvements are not started by this documentation task.
