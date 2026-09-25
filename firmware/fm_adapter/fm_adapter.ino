//최종코드에 주석 추가 버전
#include <Adafruit_TinyUSB.h> // 안드로이드 커널과 USB 통신을 가로채기 위한 핵심 프로토콜 스택 라이브러리 선언
#include <Wire.h>             // Si4703 칩셋과의 물리 레지스터 동기화를 위한 하드웨어 I2C 통신 라이브러리 선언
#include <SI470X.h>           // Si4703 하드웨어의 저전력 DSP 제어 명령어가 캡슐화된 범용 드라이버 라이브러리 선언

#define RESET_PIN 2           // Si4703 칩셋의 물리 리셋 핀 매핑 (부팅 시 레지스터 초기화 및 동기화를 위한 핵심 핀)
#define SDA_PIN   8           // RP2040 하드웨어 I2C 데이터 직렬 버스 라인을 GPIO 8번으로 지정 및 물리 회로 결합
#define SCL_PIN   9           // RP2040 하드웨어 I2C 클록 동기화 버스 라인을 GPIO 9번으로 지정 및 물리 회로 결합
#define DUMMY_INT 4           // 라이브러리 내부 강제 초기화를 방지하기 위해 할당된 물리적 비연결 수동 더미 인터럽트 핀

SI470X rx;                    // Silicon Labs Si4703 FM 튜너 하드웨어를 제어하기 위한 고유 객체 인스턴스 생성

// 앱 규격
// 107.7MHz = 10770
uint16_t current_freq = 10770; // 안드로이드 순정 FMRadioService가 주파수 전송 시 사용하는 정수형(mHz) 통신 데이터 포맷 변수

uint8_t current_vol = 7;      // 안드로이드 호스트 앱의 초기 볼륨 스케일(0~15) 값을 수용하는 하드웨어 동기화 변수

// 비동기 명령 처리
volatile uint8_t pending_cmd = 0; // USB 메인 루프 블로킹 및 탈랍을 원천 차단하기 위해 명령을 임시 저장하는 비동기 인터럽트 플래그
volatile uint16_t target_val = 0; // 비동기 제어 환경 하에서 목적지 주파수 및 타깃 제어 상수 데이터를 보존하는 휘발성 레지스터 변수

// notify 상태
volatile uint8_t notify_state = 0; // 엔드포인트 0x85번 전송 주기를 분할하여 안드로이드 커널에 2단계 상태 변화를 알리는 상태 머신 변수

// seek 상태
static unsigned long seek_start_time = 0; // 채널 탐색(SEEK) 시 메인 루프 지연으로 인한 USB 연결 드랍을 막기 위한 시간 측정용 틱 변수
static bool waiting_for_seek = false;     // Si4703 내부 레지스터가 탐색을 끝낼 때까지 하드웨어 상태를 비동기로 대기시키는 플래그 변수

// 상태값
bool last_seek_success = false; // 자동 채널 탐색 성공 여부를 기록하여 안드로이드 QUERY 명령 시 폰으로 패킷을 리턴하는 플래그
uint8_t last_rssi = 0;          // RF Down-conversion을 거쳐 칩셋 내부 DSP 레지스터에 기록된 최종 신호 감도 측정값 저장 변수

extern "C"
{
  // TinyUSB 내부에 봉인된 저전력 하드웨어 엔드포인트 패킷 다이렉트 전송 C언어 API 함수 강제 링킹 선언
  bool usbd_edpt_xfer(
    uint8_t rhport,
    uint8_t ep_addr,
    uint8_t *buffer,
    uint16_t total_bytes
  );

  // 지정한 엔드포인트 파이프가 현재 호스트와 통신 중(Busy)인지 물리적 상태를 실시간 트레이싱하는 커널 함수
  bool usbd_edpt_busy(
    uint8_t rhport,
    uint8_t ep_addr
  );
}

// ----------------------------------------------------
// 안전 튜닝
// ----------------------------------------------------
void safeTune(uint16_t freq_val)
{
  uint16_t channel = 0;

  // 시행착오 반영: 호스트 정수 데이터 포맷(10770)과 일반 물리 주파수 포맷 간의 오차를 구분하는 데이터 가공 분기점
  if (freq_val > 5000)
  {
    // [시행착오 교정] 라이브러리의 100kHz 연산 버그를 타파하기 위해 국내 표준인 200kHz 격자 상수를 역산 적용하는 물리 수식
    channel = (freq_val - 8750) / 20; 
  }
  else
  {
    // 875.0 물리 형식으로 직접 데이터 유입 시 격자 오차 방지를 위한 예외 처리 필터 및 수식 매핑
    channel = (freq_val - 875) / 2; 
  }

  rx.setChannel(channel); // 교정 연산이 완료된 물리 채널 레지스터 인덱스 값을 Si4703 하드웨어 버스에 최종 전달 및 동기화
}

// ----------------------------------------------------
// 삼성 USB 인터페이스
// ----------------------------------------------------
// USB Tree Viewer 분석 결과 도출된 정품 수신기의 인터페이스 구조와 헥사 배열을 완벽하게 재현하기 위한 상속 클래스 정의
class SamsungHybridInterface : public Adafruit_USBD_Interface
{
public:

  virtual uint16_t getInterfaceDescriptor(
    uint8_t itfnum,
    uint8_t* buf,
    uint16_t bufsize
  )
  {
    // [시행착오의 정점] 안드로이드 표준 파서의 UsbDescriptorParser 예외를 회피하기 위해 정품 배열과 완벽 일치시킨 헥사 명세
    uint8_t desc[] =
    {
      // 인터페이스 2번: 삼성 고유 통신 패킷 교환을 위한 벤더 전용(Vendor Specific) 하드웨어 엔트리 포인트 선언
      9,
      TUSB_DESC_INTERFACE,
      2,
      0,
      0,
      TUSB_CLASS_VENDOR_SPECIFIC,
      0,
      0,
      0,

      // 인터페이스 3번: 기기 FOTA 및 추가 제어 상태를 동기화하기 위한 정품 규격 벤더 인터페이스 동합 배치
      9,
      TUSB_DESC_INTERFACE,
      3,
      0,
      0,
      TUSB_CLASS_VENDOR_SPECIFIC,
      0,
      0,
      0,

      // 인터페이스 4번: 실질적인 FM 라디오 데이터 패킷을 실시간 송수신하기 위해 삼성 앱이 바라보는 타깃 벤더 엔트리
      9,
      TUSB_DESC_INTERFACE,
      4,
      0,
      1,
      TUSB_CLASS_VENDOR_SPECIFIC,
      0,
      0,
      0,

      // [핵심 교정] 엔드포인트 0x85번(Interrupt IN) 명세 및 안드로이드 가상 드라이버의 패킷 수용을 위한 주기 설정(4ms)
      7,
      TUSB_DESC_ENDPOINT,
      0x85,
      TUSB_XFER_INTERRUPT,
      5,
      0,
      4
    };

    // 버퍼 메모리 크기가 우리가 수동으로 설계한 디스크립터 총 바이트 수보다 작을 경우 예외 차단 및 드랍 처리
    if (bufsize < sizeof(desc))
      return 0;

    // 가공된 순수 정품 인터페이스 바이트 명세 데이터를 아두이노 USB 컨피규레이션 커널 메모리에 그대로 복사 주입
    memcpy(buf, desc, sizeof(desc));

    return sizeof(desc); // 인젝션 완료된 최종 디스크립터의 바이트 길이를 반환하여 파서에 최종 등록
  }
};

SamsungHybridInterface samsung_hybrid; // 삼성 정품 프로토콜 위장 인터페이스 객체 인스턴스 생성 및 하드웨어 메모리 할당

// 역컴파일(Decompilation) 도구 분석 결과 알아낸 정품 앱 고유의 로우 레벨 벤더 요청(bRequest) 식별 번호 상숫값 정의
enum BesCmd
{
  SET   = 161, // 호스트 스마트폰이 외부 라디오 하드웨어 레지스터 값을 강제 변경(Write)할 때 던지는 고유 식별 번호
  GET   = 162, // 호스트 스마트폰이 현재 하드웨어의 볼륨 및 주파수 설정 상태를 질의할 때 사용하는 식별 번호
  QUERY = 163  // 호스트 스마트폰이 현재 라디오 하드웨어의 RSSI 신호 강도 및 수신 성공 여부를 실시간 요청할 때 쓰는 번호
};

// ----------------------------------------------------
// SETUP
// ----------------------------------------------------
void setup()
{
  // [대성공] 안드로이드 커널이 장치 식별을 수행할 때 Waveshare 기본값을 무시하고 정품으로 오인하도록 VID/PID 스푸핑 강제 설정
  USBDevice.setID(0x04E8, 0xA05B);

  // 안드로이드 하드웨어 관리자 장치 문자열에 표기될 제조사 이름을 정품 데이터 스트링인 "Samsung"으로 강제 복사 각인
  USBDevice.setManufacturerDescriptor("Samsung");
  // 안드로이드 시스템 서비스 내부 매핑 규칙을 통과하기 위해 제품 식별자 문자열 스트링을 정품 명칭으로 완벽 클로닝
  USBDevice.setProductDescriptor("Samsung USB C Earphone");

  // 우리가 수동으로 헥사 바이트를 쪼개어 정교하게 직조한 삼성 벤더 인터페이스 아키텍처를 USB 통신 스택에 최종 주입
  USBDevice.addInterface(samsung_hybrid);

  // [시행착오 해소] 안드로이드 폰이 기존 USB 드라이버 캐시를 완전히 파괴하고 새 디스크립터를 강제로 다시 파싱하도록 유도하는 버스 디태치 시퀀스
  USBDevice.detach();
  delay(100); // 버스 리셋 신호가 안드로이드 커널 레이어에 물리적으로 완전히 도달하여 각인될 때까지 확보된 필수 지연 시간
  USBDevice.attach(); // 하드웨어 라인을 물리적으로 다시 연결하여 폰 내부의 UsbAlsaManager 및 수신기 서비스 강제 기동

  // I2C
  Wire.setSDA(SDA_PIN); // RP2040 칩 내부의 하드웨어 I2C 컨트롤러의 데이터 핀 매핑을 물리 회로 GPIO 8번으로 지정 결합
  Wire.setSCL(SCL_PIN); // RP2040 칩 내부의 하드웨어 I2C 컨트롤러의 클록 동기화 핀 매핑을 물리 회로 GPIO 9번으로 지정 결합

  Wire.begin(); // 마이크로컨트롤러의 주 제어 버스로 사용될 하드웨어 2선식 I2C 마스터 엔진 최종 가동

  Wire.setClock(100000); // Si4703 레지스터 통신 타이밍의 안정성을 확보하기 위해 전송 속도를 표준 표준 100kHz 모드로 클록 고정

  // SI4703
  rx.setup(RESET_PIN, DUMMY_INT); // 하드웨어 리셋 라인(GPIO 2)에 물리 리셋 파동을 인가하여 믹서 및 내부 레지스터 전체 초기화 구동

  // 중요
  // 네 환경에서는 0이 정상 동작
  rx.setBand(0); // 주파수 수신 대역 범위를 한국 표준 FM 규격 대역($87.5 \sim 108.0 \text{ MHz}$)으로 설정하는 레지스터 비트 마스크

  // 200kHz spacing
  rx.setSpace(0); // [수식 정합] 칩셋 내부 DSP 엔진의 주파수 스페이싱 격자를 국내 표준 규격인 200kHz 이격 모드로 강제 지정 고정

  rx.setMute(true); // 전원 인가 시 초기 과전류 및 하드웨어 노이즈가 호스트 사운드 라인으로 유입되는 것을 방지하기 위해 오디오 차단

  rx.setVolume(0); // 하드웨어 아날로그 가인(Gain) 출력을 최소화하여 부팅 초기 쇼크 펄스로부터 시스템 내부 회로를 보호

  safeTune(current_freq); // 안드로이드 호스트가 기대하는 초기 구동 주파수인 107.7MHz 대역으로 하드웨어 채널 레지스터 정밀 조율
}

// ----------------------------------------------------
// LOOP
// ----------------------------------------------------
void loop()
{
#if defined(ARDUINO_ARCH_RP2040)
  TinyUSBDevice.task(); // 안드로이드 커널과의 데이터 인터럽트 및 제어 요청 패킷을 실시간 폴링 처리하는 USB 백그라운드 코어 구동
#endif

  // ------------------------------------------------
  // SEEK 시작
  // ------------------------------------------------
  // 비동기 실행 매커니즘: 호스트가 자동 채널 탐색을 요청하고(pending_cmd == 7) 현재 탐색 작업이 진행 중이 아닐 때 진입
  if (pending_cmd == 7 && !waiting_for_seek)
  {
    if (target_val == 1)
    {
      rx.seek(0, 1); // Si4703 하드웨어 레지스터의 SEEK 비트를 활성화하여 상위 주파수 대역 방향으로 자동 전파 탐색 트리거
    }
    else
    {
      rx.seek(0, 0); // Si4703 하드웨어 레지스터의 SEEK 비트를 활성화하여 하위 주파수 대역 방향으로 자동 전파 탐색 트리거
    }

    seek_start_time = millis(); // 칩셋이 대기 전파를 스캔하는 동안 메인 루프가 멈춰 USB 연결이 끊어지는 것을 막기 위한 현재 시간 기록

    waiting_for_seek = true; // 하드웨어 레지스터 스캔이 완료될 때까지 루프가 다른 비동기 작업을 수행하도록 상태 머신 전환

    pending_cmd = 0; // 처리가 완료된 비동기 명령 플래그를 초기화하여 중복 하드웨어 트리거 현상을 원천 방지
  }

      // [수 주간의 삽질 극복] 하드웨어 지연(최소 60ms)을 확보하여 칩셋 내부 DSP가 안정적으로 주파수를 복조하고 레지스터를 갱신할 시간을 보장
      if (waiting_for_seek && millis() - seek_start_time > 60)
      {
          // 기존: uint16_t raw_f = rx.getFrequency();
          uint16_t fake_f = rx.getFrequency(); // [버그 포착] 범용 라이브러리가 하드웨어 레지스터 인덱스를 서구권 100kHz 단위로 강제 변환하여 뱉어낸 왜곡된 주파수 데이터 값

          // 가짜 주파수에서 칩셋의 진짜 채널 번호를 역추출
          uint16_t channel = (fake_f - 8750) / 10; // [수식 구현] 왜곡된 데이터 역산을 통해 Si4703 내부 레지스터 고유의 물리 채널 인덱스 값을 순수하게 추출하는 핵심 수학 공식

          // 200kHz(한국 규격)를 곱해서 진짜 주파수 복원
          uint16_t real_f = 8750 + (channel * 20); // [선형 복원] 물리 인덱스에 국내 표준 200kHz 상수를 결합하여 데이터 정합성을 확보하는 최종 주파수 복원 수식

          // 복원된 진짜 주파수 사용
          current_freq = real_f; // 안드로이드 순정 라디오 앱과 1:1 패킷 동기화를 수행하기 위해 보정 완료된 최종 실제 주파수를 변수에 갱신

          last_rssi = rx.getRssi(); // RF Down-conversion 결과물인 내부 DSP 수신 신호 강도 레지스터 값을 가로채어 변수에 보존

          last_seek_success = (last_rssi > 15); // 신호 감도 임계값(15dBm)을 물리 기준으로 삼아 자동 채널 탐색의 최종 록온(Lock) 성공 여부를 판정

          notify_state = 1; // 폰에게 인터럽트 엔드포인트(0x85)를 통해 "현재 주파수 탐색 완료되었으니 데이터를 가져가라"고 알리는 2단계 Notify 시퀀스 시동

          waiting_for_seek = false; // 자동 채널 탐색 상태 머신을 종료하고 다음 제어 명령을 수용할 수 있도록 하드웨어 대기 모드로 복귀
      }
  

  // ------------------------------------------------
  // DIRECT TUNE
  // ------------------------------------------------
  // 사용자가 순정 라디오 앱 UI 상에서 다이렉트로 주파수 다이얼을 변경했을 때(pending_cmd == 9) 진입하는 다이렉트 제어 블록
  if (pending_cmd == 9)
  {
    current_freq = target_val; // 호스트가 패킷에 실어 보낸 타깃 물리 주파수 데이터를 시스템 전역 변수에 즉시 동기화

    safeTune(current_freq); // 격자 밀림 오차가 수정된 하드웨어 safeTune 제어 함수를 호출하여 Si4703 채널 레지스터를 강제 재조율

    last_rssi = rx.getRssi(); // 수동 튜닝이 완료된 시점의 물리 전파 공간의 신호 강도 데이터를 레지스터에서 즉시 추출하여 보존

    last_seek_success = true; // 다이렉트 강제 튜닝 명령이므로 주파수 탐색 및 정합 상태 플래그 플래그를 무조건 성공 상태로 고정

    notify_state = 1; // 변동된 주파수 및 RSSI 물리 데이터를 폰에 송신하기 위해 2단계 엔드포인트 통지상태 인터럽트를 1단계로 트리거

    pending_cmd = 0; // 다이렉트 튜닝 비동기 제어 명령 처리가 완결되었으므로 제어 플래그 변수를 무부하 초기화 상태로 복귀
  }

  // ------------------------------------------------
  // NOTIFY STEP 1
  // ------------------------------------------------
  // 2단계 Notify 프로토콜 - 1단계: 0x85번 인터럽트 IN 파이프가 비어있고 통지 상태가 활성화되었을 때 상태 변화 예고 프레임 주입
  if (notify_state == 1 &&
      !usbd_edpt_busy(0, 0x85))
  {
    // [시리얼 트레이싱 성과] 정품 이어폰 패킷 분석을 통해 알아낸 상태 변동 알림용 고정 바이트 시퀀스 데이터 배열 정의
    static uint8_t step1[5] =
    {
      0x01, // 패킷 유형 식별 헤더 바이트
      0x00, // 하위 명령 상태 식별 바이트
      0x08, // 상태 천이 통지 커맨드 코드
      0x00, 
      0x00
    };

    // TinyUSB 커널 드라이버 로우 레벨 API를 직접 제어하여 안드로이드 호스트 수신기 서비스 파이프라인에 5바이트 통지 패킷을 물리적 전송
    usbd_edpt_xfer(0, 0x85, step1, 5);

    notify_state = 2; // 예고 프레임 전송이 완료되었으므로 실질적인 전파 데이터 패킷을 전송하기 위해 상태 머신을 2단계로 천이
  }

  // ------------------------------------------------
  // NOTIFY STEP 2
  // ------------------------------------------------
  // 2단계 Notify 프로토콜 - 2단계: 1단계 통지가 안드로이드 커널에 수용 완료되고 파이프가 다시 대기(Ready) 상태로 전환되었을 때 진입
  else if (notify_state == 2 &&
           !usbd_edpt_busy(0, 0x85))
  {
    // 실질적인 하드웨어 측정 물리 데이터(보정 완료된 진짜 주파수 + 실시간 수신 신호 강도 RSSI)를 패킷 프레임에 적재
    uint8_t step2[5] =
    {  
      0x01, // 데이터 전송 프레임 헤더 바이트 고정
      0x01, // 데이터 세그먼트 식별자 코드

      (uint8_t)(current_freq & 0xFF),        // [바이트 분할] 16비트 정수형 주파수 데이터의 하위 8비트를 잘라내어 패킷 2번 인덱스에 적재
      (uint8_t)((current_freq >> 8) & 0xFF), // [바이트 분할] 16비트 정수형 주파수 데이터의 상위 8비트를 비트 시프트하여 패킷 3번 인덱스에 적재

      last_rssi // RF Down-conversion 필터링의 결과물인 실시간 전파 신호 강도(RSSI) 물리 수치 데이터를 최종 4번 인덱스에 결합
    };

    // 오차가 보정된 주파수와 하드웨어 측정 데이터가 담긴 최종 5바이트 데이터 프레임을 0x85번 엔드포인트를 통해 안드로이드 호스트로 물리 송신
    usbd_edpt_xfer(0, 0x85, step2, 5);

    notify_state = 0; // 2단계에 걸친 인터럽트 주파수 변동 알림 프로토콜이 성공적으로 완결되었으므로 통지 상태 머신을 무부하 초기화 상태로 닫음
  }
}

// ----------------------------------------------------
// AUDIO CALLBACK
// ----------------------------------------------------
// 안드로이드 표준 오디오 드라이버(snd-usb-audio) 및 ALSA 커널이 오디오 스트리밍 패킷 수신 완료 시 자동 트리거하는 필수 드라이버 하위 콜백 함수
extern "C"
bool tud_audio_rx_done_cb(
  uint8_t rhport,
  uint16_t n_bytes
)
{
  return true; // 가상 드라이버의 크래시 및 버퍼 오버플로를 막기 위해 데이터가 정상 수용되었음을 나타내는 ACK 불리언 상숫값을 고정 반환
}

// ----------------------------------------------------
// USB CONTROL CALLBACK
// ----------------------------------------------------
// 안드로이드 내장 FMRadioService 앱이 0번 컨트롤 엔드포인트를 통해 하드웨어 제어 명령 패킷을 쏠 때마다 실시간 호출되는 핵심 디코딩 커널 제어 핸들러
extern "C"
bool tud_vendor_control_xfer_cb(
  uint8_t rhport,
  uint8_t stage,
  tusb_control_request_t const * request
)
{
  // USB 통신 단계 중 호스트가 명령을 인젝션하는 최초 셋업(SETUP) 단계가 아닐 경우, 무부하 통과 및 드라이버 안정 상태 유지 리턴
  if (stage != CONTROL_STAGE_SETUP)
  {
    return true;
  }

  uint8_t req_type = request->bRequest; // 안드로이드 라디오 시스템 서비스가 보낸 패킷 헥사 데이터에서 BesCmd(161, 162, 163) 제어 유형 비트를 추출

  uint16_t cmd = request->wValue; // 호스트가 패킷 헤더에 실어 보낸 고유의 라디오 하드웨어 제어 서브 커맨드 식별 번호(0, 4, 5, 7, 9)를 디코딩
  uint16_t val = request->wIndex; // 볼륨 크기, 탐색 방향, 다이렉트 타깃 주파수 등 실질적인 제어 상수 파라미터 바이트 데이터를 연산 장치로 추출

  // ------------------------------------------------
  // SET
  // ------------------------------------------------
  // 안드로이드 호스트 장치가 하드웨어의 상태 레지스터 값을 강제로 변경 및 명령을 하달하는 유형(bRequest == 161)일 때 진입하는 제어 코어
  if (req_type == BesCmd::SET)
  {
    switch (cmd)
    {
      // RADIO ON/OFF 제어 (서브 커맨드 식별 번호 0)
      case 0:

        if (val == 1)
        {
          rx.setMute(false); // 라디오 켜기 명령이므로 Si4703 칩 내부 오디오 신호 라인의 뮤트 레지스터를 해제하여 하향 오디오 경로를 활성화

          rx.setVolume(current_vol); // 사운드 왜곡을 예방하기 위해 이전에 사용자가 설정해 두었던 하드웨어 볼륨 레벨 상수를 레지스터에 즉시 복구 주입
        }
        else
        {
          rx.setMute(true); // 라디오 끄기 명령이므로 하드웨어 레지스터의 뮤트 비트를 강제 활성화하여 아날로그 사운드 출력을 완전히 차단
        }

        break;

      // MUTE 제어 (서브 커맨드 식별 번호 4)
      case 4:

        if (val == 1)
        {
          rx.setMute(true); // 시스템 오디오 음소거 명령이므로 Si4703 내부 DSP 아날로그 아웃풋 라인의 오디오 음성 신호 전송을 즉시 블로킹 처리
        }
        else
        {
          rx.setMute(false); // 음소거 해제 명령이므로 오디오 신호 뮤트를 풀고 현재 수신 중인 무선 주파수 음향을 출력 파이프라인으로 재방출

          rx.setVolume(current_vol); // 음소거 해제 시 사운드 폭발 노이즈를 방지하기 위해 정돈된 볼륨 스케일 데이터를 하드웨어 레지스터에 재주입
        }

        break;

      // VOLUME 제어 (서브 커맨드 식별 번호 5)
      case 5:

        current_vol = val; // 안드로이드 순정 UI 볼륨 슬라이더 조작 시 유입되는 0~15 단계의 상숫값을 시스템 볼륨 전역 변수에 즉시 갱신

        rx.setVolume(current_vol); // 갱신된 볼륨 수치를 Si4703 내부 아날로그 앰프 제어 레지스터 레벨에 직접 대입하여 즉각적인 볼륨 가인 변경 수행

        break;

      // SEEK 제어 (서브 커맨드 식별 번호 7)
      case 7:

        target_val = val; // 호스트가 명령한 탐색 방향성 파라미터 데이터(1: 상위 대역 탐색, 0: 하위 대역 탐색)를 비동기 레지스터 변수에 이식

        pending_cmd = 7; // 메인 루프가 이 명령을 감지하여 비동기 하드웨어 타이머 스캔을 가동하도록 상태 제어 비동기 플래그를 변조 트리거

        break;

      // DIRECT TUNE 제어 (서브 커맨드 식별 번호 9)
      case 9:

        target_val = val; // 호스트 앱이 패킷으로 쏘아 보낸 강제 이동 목적지 주파수 데이터(예: 10770)를 타깃 보존 변수에 안전하게 이식

        pending_cmd = 9; // 메인 루프의 비동기 제어 세그먼트가 이 플래그를 가로채어 safeTune 보정 수식 함수를 구동하도록 플래그 변조 활성화

        break;
    }

    // ACK 제어 프로토콜: 호스트가 추가 데이터 스테이지 없이 순수 커맨드 패킷만 전송한 경우(wLength == 0) 핸드쉐이크 상태 ACK 신호 즉시 전송
    if (request->wLength == 0)
    {
      return tud_control_status(rhport, request); // 안드로이드 호스트 장치에게 "명령이 성공적으로 수용 및 디코딩되었다"는 통제 응답 상태 패킷을 즉시 반환
    }
    else
    {
      static uint8_t ack_buf[64] = {1}; // 데이터 스테이지가 존재하는 전송 규격일 경우, 통신 파이프라인 규격에 의거하여 성공 비트가 담긴 버퍼 생성

      // TinyUSB 제어 엔드포인트 전송 API를 기동하여 안드로이드 호스트 시스템이 요구한 길이만큼 ACK 바이트 스트림을 물리 전송 처리
      return tud_control_xfer(
        rhport,
        request,
        ack_buf,
        request->wLength
      );
    }
  }

  // ------------------------------------------------
  // GET / QUERY
  // ------------------------------------------------
  // 안드로이드 호스트 가상 시스템 서비스가 하드웨어의 상태 데이터를 읽어가기(Read) 위해 요청하는 유형(bRequest == 162 혹은 163) 진입 블록
  else if (
    req_type == BesCmd::GET ||
    req_type == BesCmd::QUERY
  )
  {
    // GET 명령 디코딩 분기 세그먼트
    if (req_type == BesCmd::GET)
    {
      static uint8_t res_get[64] = {0}; // 호스트 스마트폰으로 데이터 패킷을 리턴하기 위한 64바이트 하드웨어 컨트롤 데이터 송신 버퍼 확보

      uint16_t data = 1; // 기본 상태 오류 방지를 위한 초깃값 매핑

      if (cmd == 8)
      {
        data = current_vol; // 호스트가 현재 장치의 볼륨 레벨을 조회(cmd == 8)할 경우 시스템 볼륨 변수 데이터를 리턴 레지스터에 적재
      }
      else if (cmd == 13)
      {
        data = current_freq; // 호스트가 현재 수신 중인 채널 주파수를 조회(cmd == 13)할 경우 보정 완료된 주파수 변수 데이터를 적재
      }

      // 16비트 정수형 상태 데이터를 64바이트 송신용 버퍼 메모리 최하단 주소 영역에 안전하게 하드웨어 바이트 단위로 분할 카피
      memcpy(res_get, &data, 2);

      // 안드로이드 순정 FMRadioService가 요청한 길이만큼 가공된 하드웨어 볼륨/주파수 상태 데이터를 컨트롤 파이프로 물리 전송 처리
      return tud_control_xfer(
        rhport,
        request,
        res_get,
        request->wLength
      );
    }

    // QUERY 명령 디코딩 분기 세그먼트: 제조사 순정 라디오 앱이 주기적으로 하드웨어 장치의 실시간 상태를 감시할 때 진입
    static uint8_t res_query[64] = {0}; // 호스트 서비스로 가공된 측정 물리 지표 데이터를 전달하기 위한 64바이트 쿼리 응답 전용 버퍼 생성

    // 0번 인덱스: 자동 채널 탐색 성공 여부 비트 주입 (성공 시 1, 실패 시 0 바이트 데이터를 인젝션하여 앱 내부 상태 머신 동기화)
    res_query[0] =
      last_seek_success ? 1 : 0;

    res_query[1] = 1; // 정품 이어폰 내부의 무선 신호 정합 상태 규격을 모사하기 위한 프로토콜 고정 상숫값 주입

    // 2번 인덱스: 현재 튜닝되어 수신 중인 주파수 데이터의 하위 8비트 값을 비트 마스킹 연산하여 패킷 프레임에 적재
    res_query[2] =
      current_freq & 0xFF;

    // 3번 인덱스: 현재 수신 중인 주파수 데이터의 상위 8비트 값을 오른쪽으로 8칸 비트 시프트 연산하여 패킷 프레임에 최종 병합
    res_query[3] =
      (current_freq >> 8) & 0xFF;

    // 4번 인덱스: RF Down-conversion 하향 주파수 변환을 거쳐 실시간 측정된 고유 안테나 수신 신호 강도(RSSI) 물리 수치 데이터를 최종 적재
    res_query[4] = last_rssi;

    // 최종 정합 연산이 완료된 64바이트 QUERY 상태 응답 버퍼 데이터를 컨트롤 파이프를 통해 안드로이드 시스템 서비스로 리턴 전송
    return tud_control_xfer(
      rhport,
      request,
      res_query,
      request->wLength
    );
  }

  return false; // 정의되지 않은 비정상 프로토콜 패킷 유입 시 드라이버 보호 및 비정상 접근 차단을 위해 폴스 상숫값 리턴 종료
}