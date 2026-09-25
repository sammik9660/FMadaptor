# Repository Working Rules

## Canonical Source Protection

먼저 PROJECT_STATE.md, README.md와 현재 사용자 요청을 읽고 실제 파일 상태를 확인한다. 상태 문서는 기록 시점의 정보이므로 이후 생성된 파일을 무시하거나 완료 작업을 반복하지 않는다.

canonical firmware는 `firmware/fm_adapter/fm_adapter.ino`다. baseline commit 전에는 코드·주석·공백·인코딩·줄바꿈을 포함해 수정하지 않는다.

보존 기준 SHA-256:
`407CC1A16C573DD90F49A0C0F213EEBBE4428F009DC69F02C21BC156A62088B8`

baseline 이후에도 코드 변경 전후 다음 순서를 따른다.

1. 현재 Git 상태와 사용자 변경사항을 확인한다.
2. 변경 목적과 사용자 요청 범위를 확인한다.
3. 기존 동작과 관련된 코드 위치를 확인한다.
4. 최소 범위만 변경한다.
5. diff를 검토한다.
6. 가능한 경우 빌드 또는 관련 검증을 수행하고, 수행하지 못한 검증을 명시한다.

동작 코드 변경 전 baseline 보존이 선행되어야 한다. baseline 생성 자체는 후속 변경을 자동으로 허가하지 않는다. 문서 작업도 기존 파일 무결성을 확인한다.

## No Unrequested Refactoring

사용자가 명시적으로 요청하지 않은 대규모 리팩터링, 함수 재구성, 파일 분할, 변수명 일괄 변경, 자동 formatting, 라이브러리 교체, USB 구조 재설계, SI4703 제어 방식 변경, 주파수 변환식 변경을 금지한다.

코드가 이상해 보여도 요청 범위 밖의 수정은 먼저 문제·근거·예상 영향을 보고하고 수정 허가를 받는다. 이미 명시적으로 허가된 변경에 대해 같은 허가를 반복 요청하지 않는다.

## Preserve Known Working Behavior

다음 구현을 근거 없이 변경하지 않는다.

- BesCmd SET=161 / GET=162 / QUERY=163
- SEEK CMD 7 / Direct Tune CMD 9
- Endpoint 0x85 Notify
- pending_cmd / target_val
- control callback → main loop 처리 구조
- SEEK 후 60ms 초과 조건
- safeTune() 및 현재 주파수 보정 로직
- SDA GPIO8 / SCL GPIO9 / RESET GPIO2
- setBand(0) / setSpace(0)

이 값들이 영원히 옳다는 뜻은 아니다. 변경이 필요하면 먼저 근거와 예상 영향을 사용자에게 설명한다. SEEK와 Direct Tune을 근거 없이 callback 내부의 동기 처리로 되돌리지 않는다.

## Evidence First

오류 분석에서는 실제 코드, Git diff, compiler output, serial log, ADB / Logcat, USB control request, tuner register / frequency / RSSI data, 실제 하드웨어 테스트 결과를 우선한다.

관찰, 사용자 확인, 추정과 재현 시험을 구분한다. 실행하지 않은 빌드나 시험을 성공했다고 기록하지 않는다. 라이브러리 버전과 내부 동작을 추측하지 않는다.

## Confirmed vs Unverified

PROJECT_STATE.md의 CONFIRMED / UNVERIFIED / NOT IMPLEMENTED 구분을 존중한다. 원본 코드 주석만으로 기능 성공을 단정하지 않는다. 미확인 기능을 README의 완료 기능으로 바꾸지 않는다.

특히 PCM USB audio, smartphone speaker audio routing, Bluetooth audio routing, complete USB Audio implementation, complete descriptor emulation, Samsung authentication bypass, universal Samsung/Android compatibility, fully non-blocking firmware를 임의로 성공했다고 표현하지 않는다.

## Scope Control

가능하면 한 번에 하나의 명확한 개발 목표를 수행한다. 사용자가 묶어서 허용한 관리 작업은 해당 범위에서 진행할 수 있다. 관련 없는 개선은 몰래 포함하지 않고 별도로 제안한다. NEXT ACTION은 하나만 제안한다.

Git 초기화·staging·commit·remote 연결·push·라이선스 선택은 현재 사용자 요청의 허용 범위를 확인한 뒤 수행한다. 문서 관리 요청을 코드 변경이나 공개 허가로 확대하지 않는다.

## Original Archive

`C:\Users\user\Desktop\어댑터`는 원본 자료 보관소다. 이 절대 로컬 경로는 원본 보호를 위한 의도된 작업 규칙 정보다.

이 repository 작업을 이유로 해당 폴더를 수정·정리·이동·삭제·이름 변경하지 않는다. 원본은 기본적으로 읽기 전용이며, 자료 복사도 사용자 허용 범위에서만 수행한다.

## Public Repository Hygiene

비밀번호, API key, token, private key, Wi-Fi credential, ADB 연결 주소, 계정·기기 식별정보를 새 문서나 로그에 노출하지 않는다. 자료 반입 전 민감정보를 확인한다. 발견 사항은 값을 재출력하지 않고 위치와 종류를 보고하며, 원본을 임의로 수정하지 않는다.

.gitignore는 이미 추적된 파일의 비밀을 제거하는 수단이 아니다. 문서·이미지는 개별 검토하며 전체 확장자를 무조건 제외하지 않는다.