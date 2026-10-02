# V2 Architecture Review — 2026-10-03

## Report context

- Report date: 2026-10-03
- Hardware-verified legacy commit: `a15fce261708a2150e12d6dc011ce3794606c2b7`
- Verified legacy merged into main by: `c94294bc829e44bb5e68e271b6d399e4fa02be47`
- Documentation baseline at analysis time: `ffe1c84dc370b0189dfd247f9e490e1a89b2bf46`
- Analysis working branch: `cleanup/remove-debug-snapshot`
- DebugSnapshot removal in this branch was uncommitted and **NOT hardware-verified** at report time.
- This report is an architecture/research snapshot, **not an implementation specification proven by hardware**.

아래 본문은 2026-10-03 채팅에 작성한 전체 engineering review 원문이다. 본문의 현재 상태·읽기 전용 작업·해시·줄 번호는 분석 시점 기준이며, 이 보고서를 저장하는 문서 commit 이후의 상태를 뜻하지 않는다. 제안된 시간 제한과 설계는 별도 구현 및 검증이 필요하다. 원문의 로컬 소스 링크는 분석 당시 설치 파일과 working copy를 가리키며, 다른 PC 또는 이후 소스에서는 동일한 경로·줄 번호를 보장하지 않는다.

---

## 1. Executive summary

**V2는 검증된 legacy를 그대로 보존하고, 별도 firmware로 개발하는 것이 적절합니다.** 핵심 변경은 파일 분할이 아니라 다음 네 가지입니다.

1. USB 콜백에서 모든 SI4703/I2C 접근을 제거한다.
2. 단일 `pending_cmd`를 고정 크기 명령 큐로 바꾼다.
3. 정상·실패·시간 제한이 명확한 tune/SEEK 상태 머신을 만든다.
4. 명령 결과와 Notify 전송을 연결하여, 다른 명령의 상태가 섞이지 않게 한다.

현재 구현은 실기에서 동작한 중요한 기준점이지만, **I2C 실패를 정상 레지스터 값처럼 취급하고 무한 STC 대기에 들어갈 수 있는 구조**가 남아 있습니다. 또한 USB 버퍼 길이 검증, 콜백과 main loop의 I2C 동시 접근, Notify 실패 처리에도 개선이 필요합니다.

이번 조사에서 구분한 증거 수준은 다음과 같습니다.

| 표기 | 의미 |
|---|---|
| **OBSERVED** | 사용자의 실제 하드웨어 관찰·시험 기록 |
| **SOURCE-CONFIRMED** | 현재 소스 또는 확인한 dependency 구현으로 확정 |
| **LIKELY** | 코드와 관찰이 뒷받침하지만 해당 실패를 직접 계측하지 않음 |
| **SPECULATIVE** | 추가 실험 없이는 판단할 수 없는 가설 |

**읽기 전용 상태를 유지했습니다.**

- Repository: `C:\Users\user\Desktop\RP2040-SI4703-FM-Adapter`
- Branch: `cleanup/remove-debug-snapshot`
- HEAD/main: `ffe1c84dc370b0189dfd247f9e490e1a89b2bf46`
- 기존 미커밋 변경: `firmware/fm_adapter/fm_adapter.ino` 한 파일
- 조사 전후 firmware SHA-256 동일:

```text
8A89D411910821DA5FFBDCFA59DD7990CCC507AB0FA57B3306BCEB793EFA1468
```

파일 수정·빌드·flash·Git 상태 변경은 수행하지 않았습니다. 아래 firmware 줄 번호는 **현재 cleanup working copy** 기준입니다. 하드웨어 검증 기준은 별도로 보존된 `a15fce2`입니다.

---

## 2. Verified invariants that V2 must preserve

### 역사에서 확인되는 결정

| 시점 | 의미 | 증거 해석 |
|---|---|---|
| `bb13482` | 원본 baseline | 최초 보존점이며 현재 검증 상태와 다름 |
| `ee34972` | 당시 작업 기록에서 pre-bounded snapshot 복구 | 소스 복구의 근거. 이 commit 자체의 안정성을 보장하지 않음 |
| `1355795` | bounded tune·boot diagnostics v4 실험 | 진단·안정성 실험. Samsung 호환성 필수 기능으로 취급하면 안 됨 |
| `416f14c` | `setup()` 두 번째 인자를 실제 SDA로 변경 | 설치 라이브러리 API 의미와 일치 |
| `fc05b69` | 선행 `Wire.begin()` 제거 | SDA reset 후 내부 begin이 I2C 기능을 복원하도록 수정 |
| `a15fce2` | GPIO4/5 사용 | 사용자 실기에서 정상 이어폰 모드·재생·튜닝 검증 |
| `c94294b` | 검증 상태를 main에 통합 | 검증 firmware 바이트 보존 |
| `ffe1c84` | 문서 정리 | 현재 HEAD |
| 현재 working tree | DebugSnapshot 제거 실험 | 기존 미커밋 상태이며 검증된 main과 구분해야 함 |

복구 근거는 [recovered-known-good.md](C:/Users/user/Desktop/RP2040-SI4703-FM-Adapter/docs/protocol/recovered-known-good.md)에 보존되어 있습니다.

### V2 초기 기준으로 고정할 항목

| 항목 | 유지할 기준 |
|---|---|
| 보드/핀 | RP2040-Zero, RESET=2, SDA=4, SCL=5 |
| I2C | 100 kHz |
| 초기화 전제 | 실제 SDA를 reset 시 LOW로 사용하고, 이후 I2C 기능으로 복원 |
| 검증 환경 | Arduino-Pico 6.1.1 / Adafruit TinyUSB 3.7.7 / PU2CLR SI470X 1.0.5 |
| USB 식별 | VID `04E8`, PID `A05B`, 현재 문자열 |
| Samsung 요청 | SET=161, GET=162, QUERY=163 |
| 명령 | SEEK=7, direct tune=9 |
| GET17 | `0` |
| Notify | Endpoint `0x85`, 기존 두 개의 5바이트 패킷 |
| 주파수 | 기본 외부 표현 `10770 = 107.70 MHz` |
| RF 설정 | BAND=0, SPACE=0, 현재 200 kHz 채널 기준 |
| 오디오 경로 | SI4703 아날로그 출력 → 이어폰 |
| 작업 분리 | USB 요청 접수와 시간이 걸리는 tuner 동작을 분리 |

**주의할 구분:** 위 항목을 포함한 *전체 구성이 동작했다*는 증거는 있습니다. 각 descriptor byte, 고정 응답값, 지연 시간이 개별적으로 반드시 필요하다는 A/B 증거까지 있는 것은 아닙니다.

GPIO8/9 실패도 **현재 배선에서 관찰한 실패**입니다. RP2040의 GPIO8/9가 원천적으로 I2C에 부적합하다는 결론은 아닙니다.

---

## 3. Current firmware architecture

현재 데이터 흐름은 다음과 같습니다.

```text
Samsung FM service
    └─ USB control request
         ├─ SET 0/4/5 → callback 안에서 SI470X → I2C
         ├─ SET 7/9   → target_val + pending_cmd
         └─ GET/QUERY → 저장된 전역 상태 반환
                              │
                            loop()
                              │
                    blocking SI470X 호출
                              │
                     주파수/RSSI 상태 갱신
                              │
                      notify_state = 1
                              │
                    0x85 Step 1 → Step 2

오디오: SI4703 → 아날로그 출력 → 이어폰
```

### 초기화

[setup()](C:/Users/user/Desktop/RP2040-SI4703-FM-Adapter/firmware/fm_adapter/fm_adapter.ino:143)은 다음 순서입니다.

1. USB 식별자·인터페이스 설정
2. detach → 100 ms 대기 → attach
3. Wire 핀·100 kHz 설정
4. `rx.setup(RESET_PIN, SDA_PIN)`
5. `setBand(0)`, `setSpace(0)`
6. mute=true, volume=0
7. `safeTune(10770)`

**USB attach가 tuner 초기화 완료보다 먼저입니다.** 따라서 setup 도중에도 host 요청이 들어올 수 있다는 전제로 설계해야 합니다.

### CMD9

[CMD9 처리](C:/Users/user/Desktop/RP2040-SI4703-FM-Adapter/firmware/fm_adapter/fm_adapter.ino:238):

```text
current_freq = target_val
→ safeTune(current_freq)
→ rx.getRssi()
→ last_seek_success = true
→ notify_state = 1
→ pending_cmd = 0
```

`current_freq`는 확인된 수신 주파수가 아니라 **먼저 저장한 요청값**입니다. 성공 플래그도 RF 수신 상태나 I2C 결과를 검사하지 않고 설정합니다.

### CMD7

[SEEK 처리](C:/Users/user/Desktop/RP2040-SI4703-FM-Adapter/firmware/fm_adapter/fm_adapter.ino:193):

```text
rx.seek(0, direction)   ← 여기 자체가 blocking
→ 반환 후 시간 기록
→ 60 ms 초과 대기
→ cached frequency 변환
→ RSSI 읽기
→ RSSI > 15이면 성공
→ Notify
```

따라서 현재의 60 ms 조건은 **SEEK를 비동기 실행시키는 장치가 아니라, blocking seek가 반환한 뒤 추가로 기다리는 조건**입니다.

### `safeTune()`와 주파수 보정

[현재 safeTune()](C:/Users/user/Desktop/RP2040-SI4703-FM-Adapter/firmware/fm_adapter/fm_adapter.ino:50):

- 입력 > 5000: `channel = (freq - 8750) / 20`
- 그 외: `channel = (freq - 875) / 2`
- 이후 `rx.setChannel(channel)`

설치 라이브러리의 [setSpace()](C:/Users/user/Documents/Arduino/libraries/PU2CLR_SI470X/src/SI470X.cpp:464)는 다음처럼 동작합니다.

```text
currentFMBand = register.SPACE = space
```

여기서는 `currentFMSpace`를 갱신해야 할 자리에 `currentFMBand`를 갱신합니다.

그 결과 현재 초기화에서는:

- 하드웨어 SPACE: 0 → 200 kHz
- 라이브러리 `currentFMSpace`: powerUp에서 설정한 1 → 100 kHz
- band: 우연히 0을 유지

**이 불일치는 SOURCE-CONFIRMED입니다.**

현재 SEEK 보정식은 이 상태에서 cached frequency를 다시 channel로 바꾸어 실제 200 kHz 주파수를 복원합니다. 따라서 보정식을 단순 삭제하면 안 됩니다. V2에서는 **라이브러리의 잘못된 내부 상태와 보정식을 함께 대체**해야 합니다.

---

## 4. Current architectural weaknesses

### 4.1 무한 STC 대기

설치된 [waitAndFinishTune()](C:/Users/user/Documents/Arduino/libraries/PU2CLR_SI470X/src/SI470X.cpp:99)에는 두 개의 무제한 반복문이 있습니다.

| 반복문 | 종료 조건 |
|---|---|
| 첫 번째 `do/while` | `STC == 1` |
| TUNE/SEEK clear 후 두 번째 `do/while` | `STC == 0` |

둘 다 timeout이 없습니다.

`setChannel()`은 먼저 60,000 µs 고정 대기 후 이 함수에 들어갑니다. 과거 정상 `safeTune ≈67 ms`는 **60 ms 고정 지연과 I2C/status 처리 시간**으로 설명 가능합니다. 정확히 모든 정상 tune이 67 ms여야 한다는 뜻은 아닙니다.

### 4.2 I2C 실패가 `0xFFFF` 상태로 들어갈 수 있음

[getStatus()](C:/Users/user/Documents/Arduino/libraries/PU2CLR_SI470X/src/SI470X.cpp:81)는 `requestFrom(..., 2)`의 반환 바이트 수를 검사하지 않습니다.

읽을 데이터가 없으면 `Wire.read()`는 `-1`을 반환하고, 이를 8비트 값으로 저장하면 `0xFF`가 됩니다. 결과적으로:

- STC=1처럼 보임
- RSSI=255처럼 보임
- 다른 status bit도 모두 켜진 것처럼 보임

이 상태에서 STC clear를 기다리면 계속 1로 읽혀 반환하지 않을 수 있습니다.

**이 경로가 존재하는 것은 확정이며, 과거 모든 hang이 이 경로였다는 것은 아직 미확정입니다.**

### 4.3 `volatile`은 명령 큐나 동기화가 아님

`target_val`과 `pending_cmd`는 별도 변수입니다.

- 새 요청이 기존 요청을 덮어쓸 수 있음
- 기존 명령 실행 중 새 요청이 들어온 뒤, 기존 처리의 `pending_cmd=0`이 새 요청까지 지울 수 있음
- command와 argument를 하나의 원자적 항목으로 읽는 보장이 없음

### 4.4 USB 콜백에서 I2C 실행

SET 0/4/5는 callback에서 직접 `rx.setMute()`와 `rx.setVolume()`을 호출합니다.

Arduino-Pico의 TinyUSB port는 software IRQ에서도 `tud_task()`를 실행합니다. 따라서 main loop의 tuner 작업을 USB callback이 중단하고 동일한 `rx` register shadow와 `Wire` 버퍼에 접근할 수 있습니다.

**단일 코어라고 안전한 구조가 아닙니다.**

### 4.5 USB task의 실행 경로도 일관되지 않음

현재 `loop()`의 `TinyUSBDevice.task()`는 직접 `tud_task()`를 호출합니다. 반면 RP2040 port의 `TinyUSB_Device_Task()`와 IRQ 경로에는 USB mutex가 있습니다.

따라서 현재 직접 호출 경로가 port의 상호배제 규칙을 우회하는 것은 소스로 확인됩니다. 실제 재진입에 의한 고장이 발생했는지는 별도 계측이 필요합니다.

### 4.6 EP0 응답 버퍼 길이 검증 부족

SET/GET/QUERY는 64바이트 버퍼를 전달하면서 길이를 `request->wLength` 그대로 지정합니다.

TinyUSB는 호출자가 제공한 실제 버퍼 크기를 알지 못합니다.

- IN 요청: 64바이트를 넘는 메모리를 읽을 가능성
- OUT 방향 요청: 버퍼 밖 쓰기 가능성
- 방향과 command별 길이도 firmware에서 충분히 제한하지 않음

이는 **SOURCE-CONFIRMED 메모리 안전성 결함**입니다. 정상 Samsung 요청에서 실제로 발생했다는 뜻은 아닙니다.

### 4.7 TinyUSB 함수 선언 불일치

Firmware는 `usbd_edpt_xfer()`를 네 인자로 수동 선언합니다.

설치된 TinyUSB 3.7.7의 실제 함수는 다음과 같습니다.

```text
usbd_edpt_xfer(rhport, ep_addr, buffer, total_bytes, is_isr)
```

다섯 번째 인자가 빠진 선언은 올바른 함수 계약이 아닙니다. 현재 RP2040 DCD 구현은 전달받은 `is_isr`를 사용하지 않으므로, 이것을 현재 실기 문제의 원인으로 단정하지는 않습니다.

V2에서는 설치된 헤더와 일치하는 호출 또는 별도 transport adapter를 사용해야 합니다.

### 4.8 Descriptor 인터페이스 수 관리

사용자 클래스는 인터페이스 번호 2/3/4를 직접 넣지만 `allocInterface()`를 호출하지 않습니다. 설치된 `addInterface()`도 이 세 인터페이스를 자동으로 계산하지 않고 내부 `_itf_count`를 사용합니다.

기본 CDC의 인터페이스 할당만 반영되면 실제 descriptor 목록과 `bNumInterfaces`가 불일치할 수 있습니다.

**소스상 불일치 경로는 확인했지만, 검증 기기의 실제 enumeration dump는 이번에 확보하지 않았습니다.** 호환성에 민감하므로 V2에서 무심코 “정상화”하지 말고 별도 실험으로 다뤄야 합니다.

### 4.9 Notify의 결과 연결과 실패 처리 부족

현재 Notify는 command별 결과를 보관하지 않고 전역 주파수/RSSI를 사용합니다.

- 다음 명령이 결과를 바꾸면 Step 2가 다른 명령의 데이터를 담을 수 있음
- transfer가 false여도 다음 state로 이동
- busy가 계속되면 끝없이 대기
- disconnect 시 미완료 명령/Notify를 구분하는 session 정보 없음

지역 변수인 Step 2 버퍼는 일반적으로 주의할 구조지만, **현재 RP2040 DCD는 이 5바이트 IN payload를 호출 중 DPRAM으로 복사합니다.** 따라서 이 경우를 확정적인 use-after-return으로 분류하지 않습니다.

---

## 5. Risk table

| 등급 | 위험 | 증거·발생 메커니즘 | 상태 | V2 처리 |
|---|---|---|---|---|
| **CRITICAL** | EP0 길이 초과 접근 | 64-byte buffer에 임의 `wLength` | SOURCE-CONFIRMED, 실제 발생 미관찰 | 방향·길이 검증, bounded response |
| **HIGH** | 영구 tune/SEEK hang | STC set/clear 무한 반복 | SOURCE-CONFIRMED, 비반환 관찰 있음 | deadline 있는 FSM |
| **HIGH** | 실패 읽기를 정상 status로 사용 | short read 미검사, `-1→0xFF` | SOURCE-CONFIRMED | 길이 검증 후에만 상태 반영 |
| **HIGH** | callback/main I2C 재진입 | 동일 Wire·register shadow 공유 | SOURCE-CONFIRMED 경로, 고장 기여는 LIKELY | 단일 I2C owner |
| **HIGH** | 명령 유실 | 한 슬롯 덮어쓰기·뒤늦은 clear | SOURCE-CONFIRMED | 고정 큐·command ID |
| **HIGH** | 큰 아날로그 소리 | 과거 초기화 실패 시 관찰 | OBSERVED, 전기적 원인 UNKNOWN | 안전 초기화, 실패 mute, 필요 시 하드웨어 gate |
| **HIGH** | 잘못된 완료 통지 | return 무시·전역 결과 혼합 | SOURCE-CONFIRMED | immutable result + Notify FSM |
| **HIGH** | 초기화 중 tuner 요청 | USB가 radio ready보다 먼저 활성화 | SOURCE-CONFIRMED | readiness gate, 요청 보관 정책 |
| **MEDIUM** | SEEK 오검출 | RSSI 단일 판정, SF/BL·AFCRL 미사용 | OBSERVED + SOURCE-CONFIRMED | 상태·품질 복합 판정 |
| **MEDIUM** | 주파수 상태 불일치 | SPACE shadow 오류와 보정 결합 | SOURCE-CONFIRMED | channel 중심 표현 |
| **MEDIUM** | USB task 재진입 | mutex 없는 직접 task 경로 | SOURCE-CONFIRMED, 실제 충돌 미관찰 | 실행 경로 일원화 |
| **MEDIUM** | TinyUSB ABI 의존 | 수동 선언의 인자 수 불일치 | SOURCE-CONFIRMED | 정확한 헤더·adapter |
| **MEDIUM** | descriptor count 불일치 | interface allocator 미사용 | SOURCE-CONFIRMED 경로 | 실제 dump 확보 후 별도 A/B |
| **MEDIUM** | reconnect의 오래된 결과 | USB session 경계 처리 없음 | SOURCE-CONFIRMED, 증상 연결 미확정 | session epoch |
| **LOW** | 진단 도구 버전 혼동 | main APK v4와 firmware v1 불일치 | SOURCE-CONFIRMED | version 명시·reader 검증 |

---

## 6. Proposed V2 architecture

```text
USB compatibility layer       미래 Buttons / Standalone
  decode / validate / cache          │
              └──────────┬───────────┘
                         ▼
                 Fixed command storage
                         ▼
                  RadioController
                  ├─ Initialization FSM
                  ├─ Tune FSM
                  ├─ Seek FSM
                  └─ Recovery FSM
                         ▼
                  Si4703 register driver
                         ▼
                    I2C bus owner
                         ▼
                       SI4703

RadioController → Immutable result → NotificationManager → USB 0x85
              └→ Published state   → GET / QUERY / 미래 OLED

모든 계층 → bounded RAM diagnostics
```

### 책임과 금지사항

| 계층 | 책임 | 하지 않을 일 |
|---|---|---|
| USB compatibility | 요청 해석, 안전한 응답, 큐 등록 | I2C, delay, tune polling |
| RadioController | 명령 순서·상태·완료 관리 | USB packet 직접 생성 |
| Tuner FSM | register-level tune/seek/init | Samsung 명령 번호 해석 |
| I2C owner | 전송·오류·버스 복구 직렬화 | 콜백에서 실행 |
| NotificationManager | 두 패킷 순서·전송 상태 | 실시간 tuner 조회 |
| Diagnostics | 고정 크기 이벤트 기록 | 실행 경로에서 무제한 출력 |

**중요:** blocking `rx.setChannel()`이나 `rx.seek()`를 FSM의 한 state에서 그대로 호출하면 목표를 달성하지 못합니다.

V2는 프로젝트 내부의 작은 **검증된 register 접근 계층**이 필요합니다. 전역 설치된 PU2CLR 라이브러리는 수정하지 않고 legacy에 계속 사용합니다. 라이브러리 코드를 참고·편입한다면 해당 저작권과 라이선스를 유지해야 합니다.

---

## 7. Command queue design

### 고정 메모리

첫 구현은 **8개 command slot**을 권장합니다.

각 slot은 다음 정보를 갖습니다.

- sequence
- USB session epoch 또는 standalone origin
- command type
- argument
- 접수 시간
- slot 상태
- 완료 결과

Slot 생명주기:

```text
FREE → QUEUED → ACTIVE → RESULT_READY → NOTIFY_PENDING → FREE
```

Notify가 없는 volume/mute 명령은 결과 기록 후 해제합니다.

이 방식은 command queue가 비었더라도 Notify 결과가 무한히 쌓이는 문제를 방지합니다. 예를 들어 command 24바이트, result 32바이트라면 8개에 약 448바이트이며, 실제 크기는 `static_assert`로 고정합니다.

### 동기화

- 첫 V2는 core0만 사용합니다.
- USB callback은 IRQ 문맥일 수 있다고 가정합니다.
- enqueue/dequeue와 상태 게시의 짧은 복사 구간만 보호합니다.
- critical section 안에서 I2C·USB 전송·대기하지 않습니다.
- ISR에서 main의 작업 완료를 기다리는 mutex/spin loop를 사용하지 않습니다.
- `volatile`만으로 큐의 일관성을 보장하지 않습니다.

GET/QUERY는 controller가 게시한 작은 일관된 상태 사본을 읽습니다. ISR reader가 중단된 writer를 기다리며 반복하는 방식은 금지합니다.

### 명령 수용과 ACK

**ACK는 명령 수용을 의미하고 완료를 의미하지 않습니다.**

- 정상 요청: slot을 확보한 뒤 ACK
- 큐가 가득 찬 경우: 성공 ACK 후 몰래 폐기하지 않음
- 제안 정책: 명령 수용 실패 시 EP0 STALL
- 단, Samsung 앱의 STALL 반응은 V2.0 필수 시험 항목

오래 기다린 명령도 별도 queue-age 제한을 가져야 합니다. 시간 초과 결과는 내부에 명시적으로 기록하며, Samsung에 어떻게 실패를 표현할지는 호환성 시험으로 확정해야 합니다.

초기 구현에서는 명령을 임의로 합치거나 “마지막 주파수만 실행”하지 않습니다. 그러한 최적화는 별도의 동작 변경입니다.

---

## 8. Tune state machine

### 데이터 모델

주파수를 세 가지로 구분합니다.

- `requested_frequency`: host 요청값
- `actual_channel`: 정상 읽기로 확인한 READCHAN
- `actual_frequency`: band/spacing으로 계산한 실제 값

내부 변환 기준:

```text
frequency_10kHz = 8750 + channel × 20
```

200 kHz grid와 87.5 MHz 시작점을 적용하면 상단의 마지막 grid 주파수는 107.9 MHz입니다. **108.0 MHz를 요청받았을 때의 legacy 처리와 앱 기대값은 별도로 검증해야 합니다.**

### 상태

| 상태 | 동작 | 실패 조건 |
|---|---|---|
| VALIDATE | 단위·범위·채널 계산 검사 | 잘못된 입력 |
| WAIT_READY | 초기화 완료 확인 | ready deadline |
| PRECHECK | 유효한 status 읽기, 이전 STC 확인 | I2C 오류·stale STC |
| CLEAR_PREVIOUS | 필요한 경우 이전 TUNE/SEEK 해제 | 쓰기 실패 |
| WAIT_STC_CLEAR | STC=0 확인 | clear timeout |
| START_TUNE | CHAN 설정, SEEK=0, TUNE=1 | 쓰기 실패 |
| WAIT_STC_SET | 주기적으로 status 확인 | set timeout |
| CAPTURE | READCHAN/status 확보 | short read |
| CLEAR_TUNE | TUNE 해제 | 쓰기 실패 |
| WAIT_FINISH | STC=0 확인 | clear timeout |
| READ_RSSI | 검증된 RSSI 확보 | I2C 오류 |
| COMPLETE | 실제 상태와 결과 게시 | — |

### 초기 시간 예산 제안

다음은 **실측된 보장값이 아니라 V2 시험 시작값**입니다.

| 항목 | 초기 제안 |
|---|---:|
| 단일 I2C transaction timeout | 10 ms |
| STC polling 간격 | 5 ms |
| TUNE set 후 초기 대기 | 기존 60 ms를 우선 유지하되 scheduler 대기로 구현 |
| STC set deadline | 명령 시작 후 250 ms |
| STC clear deadline | 250 ms |
| recovery 없는 전체 tune 제한 | 750 ms |

32바이트 읽기는 100 kHz에서 정상적으로 수 ms가 필요합니다. 10 ms가 실제 모듈의 clock stretching까지 충분한지는 시험해야 합니다.

모든 deadline 비교는 timer wrap에 안전한 경과 시간 계산을 사용합니다.

---

## 9. SEEK state machine

### 현재 SEEK의 문제

설치된 `SI470X::seek()`는:

1. register를 읽고
2. TUNE와 SEEK를 모두 설정하고
3. `waitAndFinishTune()`으로 대기하고
4. `setFrequency(getRealFrequency())`로 추가 tune까지 수행합니다.

Firmware는 그 후 RSSI 한 번으로 성공을 결정합니다.

현재 powerUp 설정은:

- SEEKTH=0
- SKSNR=0
- SKCNT=0
- AGCD=1
- DSMUTE=1

즉 **강한 station qualifier를 적용한 탐색 설정이 아닙니다.** 이것이 오검출에 기여할 가능성은 높지만, 이후 탐색 고장의 유일한 원인이라고 확정할 수는 없습니다.

### 사용할 수 있는 정보

설치된 [SI470X register model](C:/Users/user/Documents/Arduino/libraries/PU2CLR_SI470X/src/SI470X.h:280)에서 확인됩니다.

| 정보 | 역할 |
|---|---|
| STC | 작업 완료 handshake |
| SF/BL | 탐색 실패 또는 band limit |
| AFCRL | AFC rail, 유효하지 않은 channel의 근거 |
| RSSI | 신호 세기 |
| READCHAN | 실제 channel |
| SEEKTH | 탐색 RSSI threshold |
| SKSNR | 탐색 SNR qualification 설정 |
| SKCNT | impulse-noise qualification 설정 |

SKSNR는 설정값입니다. 현재 코드에 없는 “실시간 SNR 측정 API”가 있다고 가정하면 안 됩니다.

### 제안 알고리즘

```text
IDLE
→ PRECHECK / STC_CLEAR
→ CONFIGURE_SEEK
→ START_SEEK
→ WAIT_STC_SET
→ CAPTURE_STATUS_AND_CHANNEL
→ CLEAR_SEEK
→ WAIT_STC_CLEAR
→ VALIDATE_CANDIDATE
   ├─ 유효 → COMPLETE
   ├─ 무효, 예산 남음 → 다음 후보 SEEK
   └─ 실패/예산 소진 → FINAL_FAILURE
```

구체적인 규칙:

1. 시작 전에 STC=0을 확인합니다.
2. SEEK 중에는 TUNE=0을 유지합니다.
3. 현재 방향 의미를 보존합니다: `val==1`이면 up, 그 외 down.
4. 초기 wrap 정책은 현재 `SKMODE=0`을 유지합니다.
5. **SEEK를 clear하기 전에** SF/BL·READCHAN·RSSI·AFCRL을 확보합니다.
6. clear 후 STC=0까지 확인해야 다음 탐색을 시작합니다.
7. SF/BL=1을 “새 방송국 발견 성공”으로 보고하지 않습니다.
8. AFCRL=1 또는 유효하지 않은 I2C 읽기는 성공으로 처리하지 않습니다.
9. 같은 READCHAN이 반복되면 무한히 다시 탐색하지 않습니다.
10. 후보 수와 전체 시간에 상한을 둡니다.

SEEK 완료 후 SEEK bit를 clear하는 handshake는 공식 [AN230 검색 제공 본문](https://www.skyworksinc.com/-/media/Skyworks/SL/documents/public/application-notes/AN230.pdf)과도 일치합니다. 다만 직접 PDF 열기는 현재 404여서, 본 설계의 상세 register 근거는 로컬 설치 파일을 우선했습니다.

### 후보 검증

초기 비교 단계:

- chip qualifier는 기존 값과 동일하게 유지
- `SF/BL=0`
- `AFCRL=0`
- 유효한 RSSI
- 기존 조건 `RSSI > 15` 유지

그 다음 별도 calibration 단계:

- 30 ms 안정화
- 10 ms 간격으로 유효한 RSSI/status 3회 측정
- RSSI 중앙값 사용
- AFCRL 지속 여부 확인
- SEEKTH·SKSNR·SKCNT를 한 번에 하나씩 조정

처음부터 임의의 “좋은 threshold”를 확정하지 않습니다. 약한 정상 방송을 모두 제거하는 설정도 실패입니다.

초기 안전 상한 제안은 **후보 8개 또는 총 5초 중 먼저 도달하는 조건**입니다. 이는 모든 방송국을 탐색한다는 보장이 아니라 무한 진행 방지용 시험값입니다.

RDS나 stereo 여부는 필수 성공 조건으로 사용하지 않습니다.

---

## 10. I2C ownership/error model

### 단일 소유자

`I2cBusOwner`만 `Wire`를 호출합니다.

- USB callback: 금지
- Notify callback: 금지
- OLED callback: 금지
- 다른 core: 초기 V2에서는 금지
- RadioController가 main scheduler에서 요청

Register shadow 역시 tuner driver만 수정합니다.

### 읽기 계약

다음 조건을 모두 만족할 때만 성공입니다.

1. 요청한 길이와 반환 길이가 일치
2. 각 `read()`가 유효
3. 필요한 register를 모두 확보
4. 오류가 없는 경우에만 새 shadow/status 게시

실패하면 이전 정상값은 보존하되 `valid=false`, 오류와 sample age를 따로 기록합니다. 이전 값을 새로운 정상 측정처럼 반환하지 않습니다.

### 오류 종류

```text
OK
NOT_READY
WRITE_FAILED
READ_SHORT
BUS_TIMEOUT
BUS_STUCK
STC_SET_TIMEOUT
STC_CLEAR_TIMEOUT
INVALID_CHANNEL
SEEK_NO_STATION
SESSION_CANCELLED
```

Arduino-Pico Wire의 반환값은 해당 구현 기준으로 해석해야 합니다. 모든 `endTransmission()` 오류를 일반 Arduino 문서의 NACK 코드와 동일하게 단정하지 않습니다.

현재 core는 deadline 있는 I2C 함수를 사용하지만, 명시적 설정이 없으면 상속된 Stream timeout은 1,000 ms입니다. **I2C 호출 하나의 timeout이 존재하는 것과 전체 작업이 bounded인 것은 별개**입니다.

V2는 한 scheduler 실행에서 최대 한 개의 I2C transaction을 수행하고, 내부에서 polling loop를 돌리지 않습니다.

---

## 11. Recovery strategy

권장 순서는 다음과 같습니다.

```text
작업 실패
→ 결과/원인 보존
→ 안전 mute 1회 시도
→ bus 상태 확인
→ 필요한 경우 bus recovery
→ SI4703 reset + bounded 초기화
→ 원래 작업 최대 1회 재시도
→ 최종 실패 또는 성공
```

### 단계별 정책

| 단계 | 정책 |
|---|---|
| Safe mute | 짧은 timeout으로 1회 시도. ACK를 실제 무음 보장으로 표현하지 않음 |
| Bus 확인 | SDA/SCL 상태와 정상 read 가능 여부 확인 |
| Bus recovery | 모든 bus client 정지, SDA release, 제한된 SCL pulse와 STOP 시도 |
| Tuner reset | RESET=2, 실제 SDA=4를 사용한 mode selection 재수행 |
| Reinitialization | oscillator/power/band/space/mute/volume을 bounded 단계로 복원 |
| Retry | 해당 operation당 최대 1회 |
| Final failure | ERROR_MUTED 상태, 진단 유지, 자동 반복 중단 |

버스 복구는 open-drain 동작을 고려해야 합니다. SCL이 외부에서 LOW로 고정된 경우 강제로 HIGH 출력하며 계속 진행하면 안 됩니다.

재초기화 때도 이미 실행 중인 Wire에서 SDA를 GPIO LOW로 바꾸고 `Wire.begin()`만 재호출하는 오류를 반복하지 않아야 합니다. V2 bus owner가 **종료 → reset/mode selection → 재시작**을 책임집니다.

초기 총 예산 제안:

- tune + recovery + 1 retry: 최대 3초
- seek + recovery + 1 retry: 최대 12초

정확한 Samsung timeout은 미확정이므로 실측 후 줄이거나 조정해야 합니다.

### Watchdog

RP2040 hardware watchdog은 CPU가 `safeTune()` 같은 소프트웨어 반복문에 갇혀도, watchdog이 활성화되어 있고 계속 feed되지 않는다면 reset할 수 있습니다.

그러나:

- USB ISR이 계속 watchdog을 feed하면 main hang을 놓칠 수 있음
- reset은 USB 연결을 끊음
- tuner가 별도 전원으로 유지되면 MCU reset만으로 전기적 상태가 완전히 초기화되지 않을 수 있음
- 재부팅 때 아날로그 transient가 재발할 수 있음

따라서 watchdog은 **정상 오류 복구가 아니라 scheduler 자체가 정지한 경우의 최후 수단**입니다. 단순 loop 반복이 아니라 상태별 허용 시간과 실제 진행을 근거로 feed해야 합니다.

---

## 12. Samsung compatibility layer

현재 wire behavior를 별도 golden specification으로 보존해야 합니다.

| 요청 | 현재 구현 |
|---|---|
| SET 0 | `1`: unmute 후 저장 volume 적용, 그 외 mute |
| SET 4 | `1`: mute, 그 외 unmute 후 저장 volume |
| SET 5 | 저장 volume 갱신 후 tuner 적용 |
| SET 7 | SEEK 예약 |
| SET 9 | direct tune 예약 |
| 기타 SET | 별도 동작 없이 ACK 경로 도달 |
| GET 8 | 저장 volume |
| GET 13 | 저장 frequency |
| GET 17 | 0 |
| 기타 GET | 기본값 1 |
| QUERY | success, 고정 1, frequency LE, RSSI |

GET2·GET18·SET14에 별도 기능이 구현되어 있다고 표현하면 안 됩니다.

### V2 보존 방법

- Samsung command 번호와 tuner driver를 분리
- 응답 byte 생성은 한곳에 집중
- little-endian을 명시
- GET/QUERY는 I2C 없이 게시된 상태 사용
- 알려진 정상 요청의 응답 길이·padding을 보존
- 잘못된 방향·초과 길이는 안전하게 거부
- unknown command 정책은 기존 정상 trace와 대조

Volume도 요청값과 실제 적용값을 구분해야 합니다. 현재는 8비트로 잘린 값이 저장되고, 라이브러리는 15를 넘으면 적용하지 않습니다. V2에서 단순 clamp를 도입하는 것도 host-visible 변경이므로 명시적인 정책이 필요합니다.

### Playback-mode UI

현재 자료만으로 다음을 확정할 수 없습니다.

- UI 활성화가 특정 descriptor에 의존하는지
- Android audio device 상태와 FM service 초기화 순서 중 무엇이 지배적인지
- 앱의 이전 연결 상태가 영향을 주는지
- 초기 GET/QUERY 응답 시점이 영향을 주는지

따라서 tuner 개선과 descriptor/UAC/HID 변경을 같은 단계에 넣지 않습니다.

**완전 Samsung 이어폰 에뮬레이션, 인증 우회, USB PCM 재생을 V2 목표로 자동 확대하지 않습니다.**

---

## 13. Notification architecture

### 결과 고정

각 operation 완료 시 다음을 고정합니다.

```text
sequence / session
result
frequency
RSSI
quality-valid
completion timestamp
```

Notify가 이 immutable result로 패킷을 만듭니다. 전역 `current_freq`를 전송 시점에 다시 읽지 않습니다.

### 상태 머신

```text
WAIT_RESULT
→ WAIT_ENDPOINT
→ SUBMIT_STEP1
→ WAIT_STEP1_COMPLETE
→ SUBMIT_STEP2
→ WAIT_STEP2_COMPLETE
→ DONE
```

규칙:

- Step 1: `01 00 08 00 00`
- Step 2: `01 01 <freq low> <freq high> <RSSI>`
- 패킷마다 고정 수명 버퍼 사용
- 제출 false면 다음 단계로 진행하지 않음
- busy와 completion에 각각 deadline
- disconnect 시 해당 USB session 결과 취소
- 다음 명령 결과와 두 패킷을 섞지 않음

`transfer return=true`는 **전송 요청 수용**입니다. Android 앱의 `Received NOTIFY`나 tune 완료 처리를 보장하지 않습니다.

설치된 vendor driver의 일반 TX callback도 성공/실패 상세를 모두 전달하지 않으므로, 실제 사용할 completion hook의 의미를 확인해야 합니다. 필요하다면 V2 전용 USB transport adapter에서 결과를 전달하되 descriptor byte는 유지합니다.

초기 timeout 제안은 단계당 500 ms입니다. 제출 자체가 거부된 경우에만 제한된 재시도를 허용하고, 전송 완료 여부가 불명확한 패킷을 무작정 재전송하지 않습니다.

오류 상황의 Samsung 패킷 정책은 별도 검증 대상입니다. 내부 실패를 임의의 성공 Notify로 바꾸면 안 됩니다.

---

## 14. Audio transient strategy

### 현재 코드로 확인되는 순서

설치된 `powerUp()`은:

- oscillator 설정 후 대기
- `DMUTE=1`
- `DSMUTE=1`
- volume=0
- power enable
- 60 ms 대기, register read, 다시 60 ms 대기

를 수행합니다.

`DMUTE=1`은 **mute 해제**입니다. 해당 코드의 “Mutes the device” 주석과 의미가 반대입니다. 다만 volume=0도 설정하므로, 이것만으로 큰 삐 소리의 원인을 확정할 수는 없습니다.

그 뒤 firmware가 `setMute(true)`와 `setVolume(0)`을 실행합니다. 그 전에 USB callback이 unmute/volume 요청을 처리할 가능성도 있습니다.

### V2 안전 초기화

1. 가능하다면 하드웨어 audio gate를 기본 mute 상태로 유지
2. tuner 제어 요청은 접수하되 ready 전 적용 금지
3. reset/mode selection
4. 검증된 I2C 연결 확인
5. oscillator 안정화
6. 초기 power 설정부터 가능한 범위에서 mute 유지
7. band/spacing 설정
8. 초기 tune 완료와 유효한 status 확인
9. 요청된 volume을 mute 상태에서 적용
10. 준비 완료 후에만 unmute
11. 오류 발생 시 mute 상태로 유지

초기 V2에서는 AGC·softmute·de-emphasis·volume ramp를 한꺼번에 변경하지 않습니다. RF/음질 변경과 초기화 안전성 변경을 분리해야 합니다.

### Firmware가 보장할 수 없는 것

- I2C가 고장 났을 때 mute 명령의 실제 적용
- 전원 상승·하강 중 아날로그 출력 상태
- 외부 모듈의 커패시터·증폭기·접지에 의한 transient
- MCU reset 중 출력 차단

이 범위까지 강하게 보호하려면 **기본 mute 상태의 하드웨어 스위치나 amplifier shutdown 회로**가 필요할 수 있습니다. 정확한 회로 없이는 부품·핀을 확정할 수 없습니다.

---

## 15. Standalone/OLED extensibility

GPIO4/5 버스를 SSD1306과 공유하는 것은 **구조적으로 가능**합니다. 다만 실제 module 전압, pull-up 합성 저항, bus capacitance, address를 확인해야 합니다.

소유 규칙:

- OLED도 직접 `Wire`를 호출하지 않고 bus owner를 사용
- tuner 상태 변경과 recovery가 우선
- OLED 업데이트는 낮은 우선순위
- recovery/reset 중 display transaction 금지
- display 초기화가 tuner reset 이전에 `Wire.begin()`을 호출하지 않도록 통제
- 정상 상태를 OLED에 표시할 때도 tuner read를 추가하지 않고 published state 사용

128×64 framebuffer 1,024바이트를 100 kHz에서 한 번에 전송하면 대략 90 ms 이상의 bus 시간이 필요합니다. 전체 화면을 매 loop마다 갱신하지 말고 작은 조각으로 나누어야 합니다.

Buttons/encoder는 USB와 동일한 command API를 사용합니다.

권장 제어권 정책:

- Samsung 연결 중: USB 우선
- standalone mode: local controls
- mode 전환 시 새 session epoch
- 두 controller가 동시에 주파수를 바꾸지 않음

이 기능은 초기 V2 구현 범위에 넣지 않습니다.

---

## 16. Proposed source-file/module layout

아래는 설계안이며 실제 폴더를 생성하지 않았습니다.

```text
firmware/
├─ fm_adapter/
│  └─ fm_adapter.ino              # 검증 legacy 보존
└─ fm_adapter_v2/
   ├─ fm_adapter_v2.ino           # setup/loop 연결만
   └─ src/
      ├─ BoardConfig.h           # 핀·clock·고정 설정
      ├─ App.cpp/.h              # scheduler
      ├─ CommandQueue.cpp/.h     # 고정 slot/순서
      ├─ RadioController.cpp/.h  # 명령 수명·상태 게시
      ├─ RadioTypes.h            # 단위·결과·error
      ├─ TuneMachine.cpp/.h
      ├─ SeekMachine.cpp/.h
      ├─ InitMachine.cpp/.h
      ├─ RecoveryMachine.cpp/.h
      ├─ Si4703Registers.h       # 명시적 mask/bit 정의
      ├─ Si4703Driver.cpp/.h     # checked register access
      ├─ I2cBusOwner.cpp/.h
      ├─ SamsungProtocol.cpp/.h  # 요청/응답 byte 규칙
      ├─ UsbTransport.cpp/.h     # TinyUSB context·endpoint
      ├─ Notification.cpp/.h
      └─ Diagnostics.cpp/.h      # bounded RAM 기록

tests/
├─ protocol_vectors/
├─ frequency_vectors/
├─ state_machine/
└─ fault_injection/

docs/
└─ v2/
   ├─ protocol-contract.md
   ├─ state-machines.md
   └─ hardware-test-record.md
```

처음부터 모든 파일을 만들 필요는 없습니다. 각 migration 단계에서 필요한 모듈만 추가합니다.

---

## 17. Legacy-to-V2 migration plan

현재 cleanup을 자동으로 legacy 기준으로 승격하지 않습니다. 별도의 V2 작업을 시작할 때도 `a15fce2`의 검증 소스를 식별할 수 있어야 합니다.

| 단계 | 구현 범위 | 수용·실기 시험 | 회귀 중단 기준 | checkpoint |
|---|---|---|---|---|
| **V2.0** | 별도 sketch, 고정 build 환경, protocol skeleton, fake backend | enumeration·GET/QUERY·길이 검증·queue 시험 | descriptor/정상 응답이 예기치 않게 달라짐 | protocol skeleton |
| **V2.1** | checked I2C·bounded init·direct tune | 기존 정상 주파수, 반복 cold boot, STC timeout 주입 | pin/init 순서 회귀, 잘못된 주파수, 무한 대기 | bounded tuner |
| **V2.2** | 실제 결과와 2-step Notify 연결 | Samsung 이어폰 모드 tune 완료·정확한 패킷 순서 | missing/중복/뒤섞인 Notify | Samsung direct-tune parity |
| **V2.3** | SEEK handshake, 기존 threshold 기준 | 상하 탐색·wrap·no-station·반복 channel | 이후 탐색 불능, 끝없는 탐색 | bounded seek |
| **V2.3a** | station qualifier calibration | 강/약 방송·잡음·실제 채널 표 비교 | 정상 방송 과도한 누락 | calibrated seek |
| **V2.4** | bus/tuner recovery, 최대 1 retry | 제어된 I2C 오류·tuner reset·최종 실패 | reset loop·USB 무단 재연결 | bounded recovery |
| **V2.5** | 초기화/audio 상태 정책 | cold/warm/reconnect·실패 초기화 | 새로운 transient 또는 mute 해제 오류 | audio safety policy |
| **V2.6** | 선택적 standalone/OLED | USB 없음·제어권 전환·bus 부하 | USB/tuner 지연 증가 | standalone extension |

각 checkpoint는 **빌드 통과와 명시한 실기 결과가 기록된 뒤** 보존합니다. Legacy는 매 단계 계속 빌드 가능해야 하며 V2를 legacy 경로에 덮어쓰지 않습니다.

Descriptor count 수정, USB dispatch 정리, tuner FSM 전환도 가능한 한 서로 다른 checkpoint로 나누어 원인을 분리해야 합니다.

---

## 18. Test matrix

| 분야 | 시험 | 통과 기준 |
|---|---|---|
| 보존 | legacy 파일 hash | 검증 기준과 동일 |
| Build | 고정 core/library/보드 옵션 | dependency 경로·버전·artifact hash 기록 |
| EP0 | 정상 SET/GET/QUERY | golden response와 일치 |
| EP0 안전성 | 0/1/2/5/64/65-byte, 잘못된 방향 | 범위 밖 접근 없음, 명시적 거부 |
| Queue | 연속 CMD9, CMD7→CMD9, 포화 | 무음 유실·argument 혼합 없음 |
| Tune | 하단·상단·중간·grid 외 요청 | 요청/실제 channel 의미 일관 |
| STC | set 안 됨·clear 안 됨 | 정해진 시간 안에 실패 |
| I2C | short read·NACK·timeout | RSSI=255 등을 정상으로 채택하지 않음 |
| SEEK | up/down/wrap/방송 없음 | 유한 종료, SF/BL 보존 |
| SEEK 품질 | 강한 방송·약한 방송·잡음 | false positive/누락을 실제 채널 표와 비교 |
| Notify | busy·submit false·disconnect | 순서 유지, state 조기 진행 없음 |
| Session | 작업 중 USB disconnect/reconnect | 이전 결과가 새 session에 전달되지 않음 |
| 초기화 | radio ready 전 SET/GET | callback I2C 없음, 정의한 ready 정책 유지 |
| Audio | cold boot·reset·recovery | mute 순서 기록, 이상 소리 발생 여부 관찰 |
| 장기 시험 | 반복 tune/seek·연결 반복 | 메모리/큐 누수·복구 반복 없음 |
| 미래 OLED | 화면 부하 중 tune | bus 예산과 USB 응답 유지 |

초기 실기 기준으로 cold boot 20회, tune/seek 수백 회 같은 반복 시험을 제안할 수 있지만, 이는 제품 신뢰성 인증이 아닙니다. 측정 횟수와 실패 횟수를 함께 기록해야 합니다.

---

## 19. Things that should NOT be changed yet

- 현재 main의 검증 firmware
- 현재 cleanup branch의 미커밋 변경
- GPIO2/4/5와 100 kHz 기준
- legacy의 `safeTune()`·주파수 보정식
- GET17=0
- 기존 Notify byte와 순서
- USB Audio/HID/descriptor layout을 임의로 확장하는 작업
- 삼성 인증·speaker PCM routing 추가
- 전역 PU2CLR 라이브러리 수정
- Arduino-Pico 6.2.0으로의 동시 업그레이드
- SEEK threshold·AGC·softmute·spacing을 한꺼번에 변경
- watchdog을 정상 복구 흐름으로 사용하는 방식
- 기존 v1/v4 DebugSnapshot을 구분 없이 V2에 가져오는 작업

특히 **현재의 주파수 보정은 설치 라이브러리 상태 불일치를 상쇄하고 있으므로, 드라이버 대체 없이 보정만 제거하면 안 됩니다.**

---

## 20. Open questions requiring hardware evidence

1. 검증한 Samsung 모델·Android·FM 앱 버전은 무엇인가?
2. 실제 성공 상태에서 host가 받은 전체 USB descriptor byte는 무엇인가?
3. `bNumInterfaces` 불일치를 Android가 어떻게 처리하고 있는가?
4. 정상 SET/GET/QUERY의 정확한 방향·길이·호출 순서는 무엇인가?
5. queue full에 대한 STALL을 Samsung 앱이 어떻게 처리하는가?
6. tune 실패를 알릴 때 기존 QUERY/Notify 형식 중 앱이 실제로 해석하는 정보는 무엇인가?
7. 108.0 MHz와 200 kHz grid 밖 요청을 앱이 보내는가?
8. SEEK 오검출 직후 STC·SF/BL·AFCRL·READCHAN은 어떤가?
9. SI4703 module의 정확한 모델·revision·oscillator·전원 회로는 무엇인가?
10. GPIO8/9 실패의 실제 원인은 배선·접촉·핀 상태·모듈 전원 중 무엇이었는가?
11. 큰 삐 소리가 발생할 때 DMUTE/VOLUME write가 실제로 성공했는가?
12. USB 재연결이 MCU만 reset하는지 tuner 전원까지 완전히 제거하는지?
13. 아날로그 출력에 하드웨어 mute를 추가할 수 있는 지점이 있는가?
14. OLED를 추가했을 때 pull-up과 전압이 안전하게 공유되는가?

**NEXT ACTION:** 코드 변경 없이, 검증된 legacy 장치의 실제 USB descriptor와 정상 CMD9 요청·응답·Notify 한 건을 같은 시험 세션에서 수집하여 V2의 호환성 기준으로 확정합니다.
