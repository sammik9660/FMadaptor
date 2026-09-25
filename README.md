# RP2040-SI4703-FM-Adapter

RP2040과 SI4703를 사용하는 실험적 USB FM Radio Adapter입니다. Android/Samsung FMRadioService의 Vendor Control Request를 처리하고, I2C로 FM 튜너를 제어하며 주파수와 RSSI를 호스트에 전달합니다.

## Overview

스마트폰 라디오 앱이 외부 장치에 보내는 vendor control 요청과 FM 튜너 제어의 연결 방식을 조사하기 위해 만든 개인 프로젝트입니다. Adafruit TinyUSB로 USB 요청을 처리하고 SI470X API를 통해 SI4703를 제어합니다.

현재 canonical firmware는 [firmware/fm_adapter/fm_adapter.ino](firmware/fm_adapter/fm_adapter.ino)입니다. 작성자가 실제 동작을 확인한 최종 코드와 주석·공백을 제외하면 동일한 주석 추가 버전을 변경 없이 보존한 파일입니다. 아래 설명은 현재 실행 코드에 근거하며, 기존 주석의 표현을 모두 검증된 사실로 취급하지 않습니다.

## What Works

다음은 현재 코드에 구현되어 있으며 작성자의 기존 동작 확인을 바탕으로 보존한 기능입니다. 모든 기기에서의 동작이나 새로운 환경에서의 빌드 재현을 의미하지는 않습니다.

- [x] RP2040 ↔ SI4703 I2C control
- [x] `safeTune()`을 통한 FM frequency tuning
- [x] SEEK 요청 처리 및 시간 기반 상태 관리
- [x] RSSI 조회 및 마지막 결과 저장
- [x] BesCmd SET / GET / QUERY handling
- [x] 음소거 및 볼륨 제어
- [x] SEEK와 Direct Tune을 control callback에서 예약하고 main loop에서 처리
- [x] Interrupt IN Endpoint `0x85`를 통한 두 단계 notification

## Hardware

사용 구성은 RP2040 기반 보드와 SI4703 FM 튜너입니다. 정확한 보드·모듈 모델, 전원 배선, 안테나 및 아날로그 오디오 연결은 아직 문서화되지 않았습니다.

| Function | RP2040 GPIO |
| --- | --- |
| SI4703 SDA | GPIO 8 |
| SI4703 SCL | GPIO 9 |
| SI4703 RESET | GPIO 2 |

I2C 클록은 `Wire.setClock(100000)`으로 100 kHz에 설정됩니다.

`DUMMY_INT = 4`는 `rx.setup(RESET_PIN, DUMMY_INT)`의 두 번째 인자로 전달됩니다. 원본 주석은 비연결 더미 핀으로 설명하지만, 현재 저장소에는 라이브러리 구현이나 실제 배선 자료가 없어 GPIO 4를 필수 인터럽트 배선으로 안내하지 않습니다.

## Software / Dependencies

| Include | 용도 | 버전 |
| --- | --- | --- |
| `Adafruit_TinyUSB.h` | USB 인터페이스 등록, control response 및 Endpoint 처리 | 아직 문서화되지 않음 |
| `Wire.h` | I2C 통신 | 보드 코어에 포함된 버전 미확인 |
| `SI470X.h` | SI4703 초기화, 채널·SEEK·음소거·볼륨·RSSI 제어 | 배포본·버전·로컬 수정 여부 미확인 |

펌웨어는 Arduino 방식의 `setup()`과 `loop()`를 사용합니다. `ARDUINO_ARCH_RP2040`이 정의된 경우 main loop에서 `TinyUSBDevice.task()`를 호출합니다.

Arduino 스케치 진입 파일은 `firmware/fm_adapter/fm_adapter.ino`입니다. 정확한 Arduino IDE, RP2040 core, 보드 선택 및 USB Stack 설정은 아직 문서화되지 않았으므로 재현 가능한 빌드·업로드 절차는 보류합니다. 라이브러리 소스와 빌드 결과물도 현재 저장소에 포함되어 있지 않습니다.

## Protocol Overview

펌웨어는 VID `0x04E8`, PID `0xA05B`를 설정하고, 제조사 문자열 `Samsung`과 제품 문자열 `Samsung USB C Earphone`을 사용합니다. 이는 코드에 설정된 식별값이며, 공식 인증이나 정품 descriptor 전체 재현을 뜻하지 않습니다.

`SamsungHybridInterface`는 vendor-specific 인터페이스 번호 2, 3, 4를 기술하고, 인터페이스 4에 Interrupt IN Endpoint `0x85`를 둡니다. 해당 Endpoint의 descriptor는 최대 패킷 크기 5바이트와 `bInterval = 4`를 지정합니다.

control callback은 다음 필드를 읽습니다.

| USB request field | 코드에서 사용하는 의미 |
| --- | --- |
| `bRequest` | BesCmd SET / GET / QUERY |
| `wValue` | command 번호 |
| `wIndex` | 명령 인자 |
| `wLength` | control transfer 길이 |

아래의 `0xA1`, `0xA2`, `0xA3`는 **bRequest 값**입니다.

| BesCmd / bRequest | Command / wValue | 코드 동작 |
| --- | --- | --- |
| SET = 161 / `0xA1` | 0 | 값이 1이면 음소거 해제·저장 볼륨 적용, 그 외에는 음소거. 하드웨어 전원 차단 구현은 아님 |
| SET = 161 / `0xA1` | 4 | 값이 1이면 음소거, 그 외에는 해제·저장 볼륨 적용 |
| SET = 161 / `0xA1` | 5 | 값을 `current_vol`에 저장하고 `rx.setVolume()` 호출 |
| SET = 161 / `0xA1` | 7 | SEEK 예약. 값이 1이면 main loop에서 `rx.seek(0, 1)`, 그 외에는 `rx.seek(0, 0)` 호출 |
| SET = 161 / `0xA1` | 9 | Direct Tune 목표 주파수 저장 및 처리 예약 |
| GET = 162 / `0xA2` | 8 | 저장된 볼륨 반환 |
| GET = 162 / `0xA2` | 13 | 저장된 주파수 반환 |
| QUERY = 163 / `0xA3` | 별도 분기 없음 | 저장된 성공 플래그·주파수·RSSI 반환 |

SEEK 인자 1/0은 코드 주석에서 상향/하향으로 설명됩니다. 라이브러리 내부의 정확한 인자 의미는 사용한 SI470X 구현과 함께 확인해야 합니다.

SET은 `wLength == 0`이면 `tud_control_status()`, 그 외에는 첫 바이트가 1인 버퍼로 `tud_control_xfer()`를 호출합니다. 이 ACK는 SEEK나 튜닝의 하드웨어 완료를 의미하지 않습니다.

GET은 응답 버퍼의 첫 2바이트에 16비트 값을 복사합니다. command 8·13 이외에는 기본값 1을 사용합니다. 처리 분기가 없는 SET command도 ACK 경로에 도달하므로, ACK만으로 해당 명령의 지원 여부를 판단할 수 없습니다.

QUERY의 처음 5바이트는 다음과 같습니다.

| Byte | 내용 |
| --- | --- |
| 0 | `last_seek_success ? 1 : 0` |
| 1 | 고정값 1. 실제 stereo 측정값으로 해석하지 않음 |
| 2 | `current_freq` 하위 바이트 |
| 3 | `current_freq` 상위 바이트 |
| 4 | `last_rssi` |

## Firmware Architecture

SEEK 및 Direct Tune의 주된 흐름은 다음과 같습니다.

```text
Android/Samsung FMRadioService
  → USB Vendor Control Request
  → tud_vendor_control_xfer_cb()
  → target_val / pending_cmd 저장 → control ACK
  → loop()에서 SI4703 제어
  → 주파수 / RSSI 저장
  → Endpoint 0x85 Notify

GET / QUERY
  → control callback에서 저장된 상태를 control response로 반환
```

1. **초기화:** USB 식별값·인터페이스를 설정하고 detach → `delay(100)` → attach를 수행합니다. I2C와 튜너를 초기화한 뒤 `setBand(0)`, `setSpace(0)`, 음소거, 볼륨 0 및 초기 주파수 튜닝을 적용합니다. 초기 `current_freq`는 10770, 저장 볼륨은 7입니다.
2. **명령 예약:** SET command 7·9는 콜백에서 `target_val`과 `pending_cmd`만 설정하고 ACK 경로로 진행합니다.
3. **SEEK:** main loop에서 `rx.seek()`를 호출한 뒤 시간을 기록합니다. `waiting_for_seek` 상태에서 `millis() - seek_start_time > 60`이면 주파수와 RSSI를 읽습니다. 이는 호출 이후 60 ms를 초과했는지 확인하는 시간 기반 처리이며, 칩의 완료 비트 확인이 아닙니다.
4. **Direct Tune:** main loop에서 `safeTune()`을 호출한 뒤 RSSI를 읽고 `last_seek_success = true`로 설정합니다.
5. **Notify:** `notify_state`와 Endpoint busy 여부를 확인하며 첫 패킷 `01 00 08 00 00`, 다음 패킷 `01 01 <freq low> <freq high> <RSSI>`를 전송 요청합니다.

SEEK 결과의 성공 플래그는 `last_rssi > 15`로 결정됩니다. Direct Tune의 성공 플래그는 무조건 true이며 실제 수신 품질 검증 결과가 아닙니다. GET / QUERY는 새 측정을 시작하지 않고 마지막 저장 상태를 반환합니다.

이 구조는 SEEK와 Direct Tune 작업을 control callback 밖으로 옮깁니다. SET 0·4·5는 콜백에서 튜너 API를 직접 호출하고, 라이브러리 내부 대기도 확인되지 않았으므로 전체 펌웨어를 완전히 non-blocking이라고 설명하지 않습니다.

## Frequency Handling

기본 주파수 표현은 `10770 = 107.70 MHz`입니다. 이 표현에서 정수 1은 0.01 MHz, 즉 10 kHz에 해당합니다.

`safeTune(freq_val)`은 다음 정수 계산으로 채널을 만든 후 `rx.setChannel(channel)`을 호출합니다.

```cpp
// freq_val > 5000
channel = (freq_val - 8750) / 20;

// 그 외의 입력
channel = (freq_val - 875) / 2;
```

두 번째 분기는 1077을 107.7 MHz로 표현하는 입력 형식에 대응하는 계산입니다. 두 계산 모두 87.5 MHz 기준의 200 kHz 채널 간격을 사용합니다.

SEEK 이후에는 다음 보정을 수행합니다.

```cpp
uint16_t fake_f = rx.getFrequency();
uint16_t channel = (fake_f - 8750) / 10;
uint16_t real_f = 8750 + (channel * 20);
current_freq = real_f;
```

이 수식은 반환값이 87.5 MHz 기준·100 kHz 간격으로 계산되었다고 가정하여 채널 인덱스를 역산하고, 200 kHz 간격으로 복원합니다. 현재 코드에 보존된 환경별 보정이며, 모든 SI470X 버전의 동작이나 라이브러리 결함을 입증하는 설명은 아닙니다.

코드는 `setBand(0)`, `setSpace(0)`을 사용하고 원본 주석은 87.5–108.0 MHz 대역과 200 kHz spacing을 의도한다고 설명합니다. 해당 API 설정의 의미는 실제 사용한 라이브러리 버전과 함께 재확인해야 합니다.

`safeTune()`에는 입력 범위 검사나 격자 정렬 검사가 없고 정수 나눗셈을 사용합니다. Direct Tune 응답의 `current_freq`도 실제 주파수 재측정값이 아니라 요청값이므로, 임의 입력에 대한 안전성이나 정확한 튜닝 결과를 보장하지 않습니다.

## Project Structure

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

## Known Limitations

- **오디오 전달:** PCM USB audio path가 구현되어 있지 않습니다. `tud_audio_rx_done_cb()`는 true만 반환합니다. 스마트폰 스피커 재생, Bluetooth 오디오 라우팅 및 완전한 USB Audio 구현은 현재 범위 밖입니다.
- **USB 호환성:** 현재 descriptor 구성과 vendor 요청 처리는 실험 구현입니다. USB descriptor 완전 에뮬레이션이나 Samsung 하드웨어 인증 우회 성공을 주장하지 않습니다. 특정 Samsung/Android 환경에서 개발되었으며 범용 호환성은 확인되지 않았습니다.
- **환경 재현:** 정확한 보드 모델, Arduino RP2040 core, 라이브러리 버전 및 로컬 수정 여부가 아직 문서화되지 않았습니다. 현재 저장소 정리 과정에서 빌드·업로드·장치 재시험은 수행하지 않았습니다.
- **타이밍:** SI470X 내부 함수의 blocking 여부는 저장소만으로 확인할 수 없습니다. SEEK의 60 ms 초과 조건은 실제 완료 검증이 아니며, 과거 UI timeout의 원인·정확한 지속시간·해결 재현을 이 코드만으로 확정할 수 없습니다.
- **상태 표현:** SEEK 성공은 RSSI 임계값 비교이며 RSSI 단위를 여기서 dBm으로 단정하지 않습니다. Direct Tune 성공 및 QUERY byte 1에는 고정값이 사용됩니다.
- **명령 처리:** pending command는 큐가 아닌 단일 저장 공간입니다. 연속 요청으로 이전 요청이 덮어써질 수 있으며, 동시·연속 요청 처리의 견고성은 추가 검토 대상입니다.
- **입력·전송 검증:** 볼륨·주파수 입력 검증이 제한적입니다. control response는 64바이트 버퍼에 대해 요청의 `wLength`를 그대로 전달하며, Endpoint 전송 함수의 반환값 확인도 없습니다. 전송 실패·버퍼 수명·비정상 요청 처리는 코드 리뷰 대상입니다.

## Contributions

향후 Public GitHub repository로 공개할 예정입니다. 다음 분야의 분석, bug report, code review 및 Pull Request를 환영합니다.

- Vendor control protocol 분석
- SI4703 동작과 주파수 보정 검증
- Android 기기별 호환성 시험
- USB timing 및 명령 처리 개선
- 하드웨어 구성과 배선 문서화·개선

문제를 보고할 때는 보드·라이브러리 버전, Android 기기·OS·앱 정보, 재현 절차, 기대 결과와 실제 결과를 함께 남겨 주세요. 로그와 화면에서는 계정·네트워크·기기 식별정보를 제거해 주세요. 코드 변경은 동작했던 기준 코드와의 차이 및 검증 결과를 설명해 주세요.

## Disclaimer

개인 연구 및 상호운용성 실험을 위한 비공식 프로젝트입니다. Samsung 또는 Silicon Labs와 공식적으로 관련되거나 이들의 승인을 받은 프로젝트가 아닙니다.

## License

이 repository의 자체 코드는 [MIT License](LICENSE)로 제공됩니다. Copyright (c) 2026 sammik9660.

Adafruit TinyUSB, SI470X, Arduino core 등 외부 dependency에는 각각의 라이선스가 적용되며, 이 프로젝트의 MIT License가 해당 라이선스를 변경하거나 대체하지 않습니다.