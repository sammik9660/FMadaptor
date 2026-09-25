# PROJECT_STATE

최종 기록일: 2026-09-26  
현재 단계: 관리 구조 및 기존 8개 파일의 baseline preflight 완료. MIT License 설정 완료. Git 초기화와 baseline commit 이전.

이 파일은 사용자와 후속 Codex 세션을 위한 상태 인계 문서다. 외부 소개는 [README.md](README.md), 실행 구현의 기준은 [canonical firmware](firmware/fm_adapter/fm_adapter.ino)다. 새 세션은 이 문서를 읽은 뒤 실제 파일 상태와 현재 사용자 요청을 확인한다. 아래 체크리스트는 기록 시점의 상태이며 다음 작업을 자동으로 허가하는 목록이 아니다.

상태 구분:

- **CONFIRMED:** 실행 코드, 파일 검증 또는 사용자 확인에 근거한다. 근거 종류를 구분하며 코드 존재를 새로운 장치 시험 성공으로 해석하지 않는다.
- **UNVERIFIED:** 환경·측정·재현이 아직 확인되지 않았다.
- **NOT IMPLEMENTED / OUT OF SCOPE:** 현재 구현 완료 기능으로 주장하지 않는다.

## 1. Current Goal

기존에 실제 동작했던 FM Adapter를 원본 훼손 없이 정리하여 Git/GitHub 기반으로 유지보수 가능한 공개 프로젝트로 이전한다. 현재 단계에서는 기능 개선보다 기존 동작 코드 보존이 우선이다.

공개는 향후 목표다. GitHub 저장소 연결·push·공개가 완료된 상태가 아니다. 한 번에 NEXT ACTION 하나씩 진행한다.

## 2. Canonical Source

**CONFIRMED — 사용자 결정 및 이전 비교·복사 검증**

| 항목 | 확정 내용 |
| --- | --- |
| 실제 최종 동작 코드 원본 | `1. 최종, 주파수 탐색 변환 완벽 코드.txt` |
| 문서화 및 향후 canonical 기준 원본 | `1. 최종코드에 주석 추가 버전.txt` |
| 새 프로젝트 canonical firmware | `firmware/fm_adapter/fm_adapter.ino` |
| 복사 방법 | 주석 추가 버전을 내용 변경 없이 파일 복사 |
| 원본과 최초 복사본 | byte-for-byte 동일한 상태로 검증됨 |
| 펌웨어 크기 | 28,247바이트 |

복사 당시 SHA-256 및 이 문서 작성 시 재확인한 SHA-256:

```text
407CC1A16C573DD90F49A0C0F213EEBBE4428F009DC69F02C21BC156A62088B8
```

실제 동작본과 주석 추가 버전은 이전 분석에서 주석·공백을 제외한 코드가 동일함을 확인했고 사용자가 canonical 선정을 확정했다. 원본 TXT와 최초 `.ino`의 바이트 동일성은 이 두 TXT 사이의 동일성과 구별한다.

기존 `어댑터` 폴더는 새 프로젝트와 별개의 원본 보관소다. 31개 원본을 수정·삭제·이동·이름 변경하지 않는다. 최초 복사 작업 전후 원본 파일의 경로·크기·SHA-256이 일치했다. 별도 추가 백업 생성까지 완료되었다는 기록은 없다.

실제 동작 성공은 사용자의 기존 확인이다. 이번 이전 작업에서는 빌드·펌웨어 업로드·하드웨어 재시험을 수행하지 않았다. 원본 주석의 과장된 설명이나 단위 표현을 검증된 사실로 승격하지 않는다.

## 3. Confirmed Implementation

**CONFIRMED — 현재 canonical firmware를 직접 대조**

| 항목 | 코드에서 확인한 내용 |
| --- | --- |
| 대상 및 진입점 | RP2040 기반 사용 구성은 사용자 확인. `setup()` / `loop()`와 `ARDUINO_ARCH_RP2040` 조건부 `TinyUSBDevice.task()` 존재 |
| 의존성 | `Adafruit_TinyUSB.h`, `Wire.h`, `SI470X.h`; `SI470X rx`로 튜너 제어 |
| I2C | SDA GPIO8 / SCL GPIO9, `Wire.begin()`, 100000 Hz |
| RESET 및 초기화 인자 | RESET GPIO2, `DUMMY_INT=4`, `rx.setup(RESET_PIN, DUMMY_INT)` |
| USB 식별값 | Samsung VID `0x04E8` / PID `0xA05B` 설정 |
| BesCmd | SET=161, GET=162, QUERY=163; 이들은 `bRequest` 값 |
| 명령 인자 | `wValue` → cmd, `wIndex` → val |
| SET | 0: 음소거 기반 ON/OFF 처리, 4: mute, 5: volume, 7: SEEK 예약, 9: Direct Tune 예약 |
| GET | command 8: 저장 볼륨, 13: 저장 주파수. 그 외 기본값 1 |
| QUERY | 성공 플래그, 고정값 1, 주파수 하위·상위 바이트, RSSI |
| Notify | Interrupt IN Endpoint `0x85`, 두 단계 5바이트 전송 요청 |
| 튜너 설정 | `setBand(0)`, `setSpace(0)`, `safeTune()` 호출 |
| 초기 상태 | `current_freq=10770`, `current_vol=7`; setup에서 mute=true, 튜너 볼륨 0 |
| 예약 상태 | `volatile uint8_t pending_cmd`, `volatile uint16_t target_val` |
| SEEK | main loop에서 val=1이면 `rx.seek(0, 1)`, 그 외 `rx.seek(0, 0)` |
| SEEK 후 처리 | 호출 반환 후 기록한 시간에 대해 `millis() - seek_start_time > 60` 검사 |
| 주파수·RSSI | SEEK 주파수 역산 및 보정, `rx.getRssi()`, 마지막 결과 저장·응답 |
| Direct Tune | main loop의 `safeTune(current_freq)`, RSSI 조회, 성공 플래그 true 지정 |

Notify 첫 패킷은 `01 00 08 00 00`, 두 번째는 `01 01 <freq low> <freq high> <RSSI>`다. Endpoint busy 여부를 확인하고 전송을 요청하지만 전송 함수의 반환값은 확인하지 않는다.

SEEK 성공 플래그는 `last_rssi > 15` 비교다. RSSI 물리 단위를 dBm으로 단정하지 않는다. QUERY byte 1은 실제 stereo 측정 결과가 아닌 고정값 1이다. Direct Tune의 성공 플래그도 실제 수신 검증이 아닌 무조건 true다. GET / QUERY는 저장 상태를 반환하며 새 측정을 시작하지 않는다.

## 4. Important Architecture Decision

**CONFIRMED — 구현 및 보존할 설계 제약**

USB control callback에서 SEEK / Direct Tune 전체 작업이 끝날 때까지 처리하지 않는다. `tud_vendor_control_xfer_cb()`는 해당 요청의 `target_val`과 `pending_cmd`를 설정하고 ACK 경로로 진행한다. 실제 튜너 제어는 main loop가 담당한다.

```text
Vendor Control Request
  → callback: target_val / pending_cmd 설정
  → control ACK
  → main loop: SEEK 또는 Direct Tune
  → 주파수·RSSI 저장
  → Endpoint 0x85 Notify

GET / QUERY → callback에서 저장 상태 반환
```

향후 리팩터링에서 이 분리를 근거 없이 없애거나 SEEK / Direct Tune을 callback 내부 동기 처리로 되돌리지 않는다. ACK는 하드웨어 작업 완료 증명이 아니다.

완전히 non-blocking인 시스템이라는 의미는 아니다. SET 0·4·5는 callback 안에서 튜너 함수를 호출하고, setup에는 `delay(100)`이 있다. `rx.seek()` / `rx.setChannel()`의 내부 대기 여부는 미확인이다. pending 저장소는 단일 슬롯이며 큐가 아니므로 연속 요청 처리도 추가 검토 대상이다.

## 5. Frequency Handling

**CONFIRMED — 코드 계산과 표현**

기본 표현은 `10770 = 107.70 MHz`로, 정수 1이 10 kHz에 해당한다. `safeTune(freq_val)`은 다음 정수 계산 후 `rx.setChannel(channel)`을 호출한다.

```cpp
// freq_val > 5000인 경우
channel = (freq_val - 8750) / 20;

// 그 외의 경우
channel = (freq_val - 875) / 2;
```

두 번째 계산은 1077을 107.7 MHz로 나타내는 입력 형식에 대응한다. 두 계산 모두 87.5 MHz를 기준으로 200 kHz 간격의 채널 인덱스를 계산한다. 입력 범위·격자 정렬 검사는 없고 정수 나눗셈을 사용한다.

SEEK 결과 변환은 다음과 같다.

```cpp
uint16_t fake_f = rx.getFrequency();
uint16_t channel = (fake_f - 8750) / 10;
uint16_t real_f = 8750 + (channel * 20);
current_freq = real_f;
```

**UNVERIFIED — 보정의 배경과 라이브러리 동작**

위 계산은 반환값이 87.5 MHz 기준·100 kHz 간격으로 계산되었다는 가정 아래 채널 인덱스를 역산하여 200 kHz 간격으로 복원한다. 수식의 존재는 확정이나, 정확한 SI470X 구현이 없으므로 그 가정의 일반적 타당성이나 특정 라이브러리 버그를 입증한 것으로 기록하지 않는다.

`setBand(0)` / `setSpace(0)` 호출은 확정이다. 원본 주석의 87.5–108.0 MHz 및 200 kHz 설정 의도와 실제 사용 라이브러리의 API 의미는 구분한다. 현재 동작했던 보정식을 근거 없이 수정하지 않는다.

Direct Tune은 요청값을 `current_freq`에 저장하고, 실제 주파수를 다시 읽어 갱신하지 않는다.

## 6. Known Limitations

**NOT IMPLEMENTED / OUT OF SCOPE**

- PCM USB audio 전달 경로는 현재 구현되어 있지 않다. `tud_audio_rx_done_cb()`는 true만 반환한다.
- 스마트폰 스피커 / Bluetooth audio routing 및 완전한 USB Audio 구현을 완료 기능으로 주장하지 않는다.
- 이번 이전 단계에서 오디오 기능 추가, 코드 수정·리팩터링, 기능 개선을 시작하지 않는다.

**UNVERIFIED — 성공 주장 금지**

- USB descriptor 완전 에뮬레이션 성공
- Samsung 하드웨어 인증 우회 성공
- 모든 Samsung/Android 기기와의 호환성
- 전체 firmware의 완전한 non-blocking 동작
- 과거 timeout의 정확한 측정값, 원인 확정 및 해결의 재현

**CONFIRMED — 코드상 한계와 검토 항목**

- SEEK의 60 ms 초과 조건은 시간 기반 검사이며 칩의 실제 완료 비트 확인이 아니다.
- RSSI 성공 판정, Direct Tune 성공 및 QUERY 고정값은 실제 수신 품질·stereo 검증과 다르다.
- 단일 pending 슬롯, 입력값 검증, 64바이트 control 버퍼에 전달하는 `wLength`, Notify 전송 결과 및 버퍼 수명은 향후 리뷰 대상이다.
- 발견한 개선 후보는 baseline 이전에 자동 수정할 근거가 아니다.

## 7. Unverified Environment

다음은 모두 **UNVERIFIED**다.

- 정확한 RP2040 개발 보드 모델
- Arduino IDE, Arduino RP2040 core / board package 및 버전
- 보드 선택과 USB Stack 설정
- Adafruit TinyUSB 버전
- SI470X library의 정확한 출처·버전 및 수정 여부
- `rx.seek()` / `rx.setChannel()` 내부 blocking 특성
- 실제 성공 테스트에 사용한 스마트폰 모델, Android 버전 및 FMRadio 앱 버전
- 모듈 모델, 전원·안테나·아날로그 오디오 배선 및 DUMMY_INT의 실제 연결
- timeout의 정확한 측정값

과거 자료에는 약 20초와 약 30초 표현이 혼재한다. 하나를 확정 측정값으로 선택하지 않는다. 사용자 설명에 있는 개선 경험과 코드상 callback/main-loop 분리는 기록할 수 있지만, 새로운 재현 시험을 완료했다고 쓰지 않는다.

## 8. Do Not Break

- baseline commit 전 canonical firmware를 수정하지 않는다.
- 자동 리팩터링·자동 formatting·주석 수정·인코딩 또는 줄바꿈 정리를 하지 않는다.
- 동작 코드 변경 전에 반드시 baseline을 보존한다. baseline 완료 자체를 코드 변경의 자동 허가로 해석하지 않는다.
- USB callback / main-loop 처리 구조를 근거 없이 변경하지 않는다.
- 주파수 변환식을 근거 없이 수정하지 않는다.
- 원본 `어댑터` 폴더를 수정·삭제·이동·이름 변경하지 않는다.
- 미확인 기능을 README의 성공 기능으로 표현하지 않는다.
- 문서 작업 시에도 canonical firmware의 SHA-256을 확인한다.
- 사용자가 지정한 단계와 파일 범위만 작업하고 NEXT ACTION은 하나만 제안한다.

## 9. Migration Status

2026-09-26 현재 상태:

- [x] Original project inventory — 원본 31개 목록·역할 분석. 기존 .doc 본문과 문서 이미지 등 미분석 범위는 남아 있음
- [x] Canonical source identified — 사용자 확정
- [x] Canonical firmware copied without modification
- [x] Copy integrity verified — 원본 TXT와 최초 .ino 바이트 동일성 확인; 현재 SHA-256 재확인
- [x] README created — 기능 설명 유지, Project Structure만 실제 구조에 동기화
- [x] PROJECT_STATE.md created — 현재 진행 상태에 동기화
- [x] AGENTS.md created
- [x] .gitignore created
- [x] Minimal docs structure created — hardware / protocol / images 안내 README만 존재
- [x] Public repository security preflight completed — 라이선스 추가 전 8개 파일의 텍스트·민감정보 패턴·절대 경로 재검사
- [x] LICENSE decision — 사용자 결정에 따라 MIT License 적용; Copyright (c) 2026 sammik9660; LICENSE 생성
- [x] Baseline review — 라이선스 추가 전 8개 파일의 목록·제외 규칙·문서 상태·보안·펌웨어 무결성 검토 완료. 라이선스 선택 후 추가·변경 파일은 commit 전에 확인 필요
- [ ] Git initialization
- [ ] Baseline commit
- [ ] Existing GitHub repository connection — 기존 원격 저장소의 존재 여부·주소 미확인, 연결 미실행
- [ ] First push
- [ ] Documentation import — 원본 기록·발표·이미지는 아직 복사하지 않음; docs 안내 README 생성과 구분
- [ ] Refactoring / improvements — baseline 보존 후 별도 범위로 진행

현재 실제 구조:

```text
RP2040-SI4703-FM-Adapter/
├── firmware/
│   └── fm_adapter/
│       └── fm_adapter.ino
├── docs/
│   ├── hardware/
│   │   └── README.md
│   ├── protocol/
│   │   └── README.md
│   └── images/
│       └── README.md
├── README.md
├── PROJECT_STATE.md
├── AGENTS.md
├── LICENSE
└── .gitignore
```

### Baseline file inventory

현재 .gitignore 규칙을 논리적으로 대조한 포함 예정 파일은 LICENSE를 포함한 다음 9개다. Git 초기화·add·status 없이 검사했으며, 실제 index나 전역 Git ignore 설정까지 검증한 결과는 아니다.

- `.gitignore`
- `AGENTS.md`
- `LICENSE`
- `PROJECT_STATE.md`
- `README.md`
- `docs/hardware/README.md`
- `docs/protocol/README.md`
- `docs/images/README.md`
- `firmware/fm_adapter/fm_adapter.ino`

현재 존재하는 파일 중 제외 규칙에 일치하는 파일은 없다. 향후 OS·IDE 임시 파일, Python cache, Arduino/build output, 로컬 credential/secret, 지정 raw-log·추출 폴더 및 local-only/는 제외한다. 모든 TXT·PDF·이미지·INO를 일괄 제외하지 않는다. 원본 어댑터 폴더는 프로젝트 밖에 있고 자료를 반입하지 않았다.

### Public repository preflight

현재 프로젝트 파일에서 실제 비밀번호, API key, token, private key, Wi-Fi credential, ADB 연결 주소, 개인 계정 및 원본 Logcat 기기 식별정보를 발견하지 못했다. 민감정보 관련 단어와 공개 USB 식별 상수 자체는 비밀값으로 분류하지 않았다.

AGENTS.md의 원본 보관소 절대 로컬 경로는 사용자가 의도적으로 포함한 유일한 로컬 절대 경로다. 그 외 불필요한 절대 경로나 원본 민감 자료의 오복사는 발견하지 못했다. 이전 보안 검사는 라이선스 추가 전 8개 텍스트 파일에 한정되며, 향후 자료 추가 시 다시 검사한다.

README의 기능 설명은 유지하고 License 섹션과 파일 구조에 MIT License를 반영했다. 펌웨어와 관리 규칙·제외 규칙·docs 안내 파일은 변경하지 않았다. 빌드·업로드·하드웨어 재시험 및 Git 작업은 수행하지 않았다.

### License check

라이선스 생성 전 8개 파일을 검사했다. Adafruit TinyUSB, SI470X, Arduino core의 소스는 직접 포함되어 있지 않으며, 펌웨어는 include·타입·API를 통해 외부 dependency를 사용한다. TinyUSB 함수 선언은 있으나 해당 라이브러리 함수 구현은 포함하지 않는다. 별도 제3자 copyright / license / SPDX header 및 직접 복제된 라이브러리 구현의 명백한 증거는 발견하지 못했다. 로컬 파일 검사만으로 모든 코드 조각의 작성 출처를 입증한 것은 아니다.

명백한 충돌 요소가 발견되지 않아 사용자 결정에 따라 자체 코드에 MIT License를 적용했다. Copyright (c) 2026 sammik9660. 표준 본문은 https://opensource.org/license/mit 와 대조했으며, LICENSE에 축약 없이 기록했다. 외부 dependency의 라이선스는 변경하거나 대체하지 않는다.

## 10. Next Action

**Git 초기화 및 baseline commit 생성**

별도 사용자 요청으로 진행한다. 이번 단계에서는 Git 작업이나 코드 개선을 수행하지 않았다.
