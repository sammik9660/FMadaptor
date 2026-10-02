#include <Adafruit_TinyUSB.h>
#include <Wire.h>
#include <SI470X.h>

#define RESET_PIN 2
#define SDA_PIN   8
#define SCL_PIN   9
#define DUMMY_INT 4

SI470X rx;

// 앱 규격
// 107.7MHz = 10770
uint16_t current_freq = 10770;

uint8_t current_vol = 7;

// 비동기 명령 처리
volatile uint8_t pending_cmd = 0;
volatile uint16_t target_val = 0;

// notify 상태
volatile uint8_t notify_state = 0;

// seek 상태
static unsigned long seek_start_time = 0;
static bool waiting_for_seek = false;

// 상태값
bool last_seek_success = false;
uint8_t last_rssi = 0;

// ----------------------------------------------------
// CMD9 debug snapshot (RAM only)
// ----------------------------------------------------
static const uint32_t DEBUG_SNAPSHOT_MAGIC = 0x47424446; // "FDBG" little-endian
static const uint16_t DEBUG_SNAPSHOT_VERSION = 1;
static const uint8_t DEBUG_SNAPSHOT_REQUEST = 0xD9;
static const uint16_t DEBUG_SNAPSHOT_VALUE = 0x464D; // "FM"
static const uint8_t DEBUG_SNAPSHOT_SLOT_COUNT = 4;
static const uint8_t DEBUG_STAGE_COUNT = 13;

enum DebugStage : uint8_t
{
  DEBUG_CMD9_CALLBACK = 0,
  DEBUG_CMD9_MAIN_LOOP,
  DEBUG_SAFE_TUNE_ENTERED,
  DEBUG_SAFE_TUNE_RETURNED,
  DEBUG_RSSI_ENTERED,
  DEBUG_RSSI_RETURNED,
  DEBUG_NOTIFY_STATE_SET,
  DEBUG_NOTIFY_STEP1_CHECKED,
  DEBUG_NOTIFY_STEP1_ATTEMPTED,
  DEBUG_NOTIFY_STEP1_RETURNED,
  DEBUG_NOTIFY_STEP2_CHECKED,
  DEBUG_NOTIFY_STEP2_ATTEMPTED,
  DEBUG_NOTIFY_STEP2_RETURNED
};

#pragma pack(push, 1)
struct DebugSnapshot
{
  uint32_t magic;
  uint16_t format_version;
  uint16_t snapshot_size;
  uint32_t boot_session_id;
  uint32_t generation;
  uint32_t cmd9_sequence;
  uint16_t target_frequency;
  uint16_t reached_flags;
  uint32_t stage_micros[DEBUG_STAGE_COUNT];
  uint8_t step1_busy;
  uint8_t step2_busy;
  uint8_t step1_transfer_return;
  uint8_t step2_transfer_return;
  uint8_t snapshot_rssi;
  uint8_t notify_state_after_set;
  uint8_t notify_state_after_step1;
  uint8_t notify_state_after_step2;
  uint8_t reserved[4];
};
#pragma pack(pop)

static_assert(sizeof(DebugSnapshot) == 88, "DebugSnapshot wire format changed");

static DebugSnapshot debug_snapshots[DEBUG_SNAPSHOT_SLOT_COUNT];
static DebugSnapshot debug_response;
static uint32_t debug_boot_session_id = 0;
static uint32_t debug_generation = 0;
static uint32_t debug_cmd9_sequence = 0;
static uint8_t debug_latest_slot = 0xFF;
static bool debug_track_notify = false;

static DebugSnapshot* activeDebugSnapshot()
{
  if (debug_latest_slot >= DEBUG_SNAPSHOT_SLOT_COUNT)
    return nullptr;

  return &debug_snapshots[debug_latest_slot];
}

static void beginDebugWrite(DebugSnapshot* snapshot)
{
  snapshot->generation = ++debug_generation;
}

static void endDebugWrite(DebugSnapshot* snapshot)
{
  snapshot->generation = ++debug_generation;
}

static void startCmd9DebugSnapshot(uint16_t frequency)
{
  debug_latest_slot =
    (debug_latest_slot >= DEBUG_SNAPSHOT_SLOT_COUNT - 1) ? 0 : debug_latest_slot + 1;

  DebugSnapshot* snapshot = &debug_snapshots[debug_latest_slot];

  memset(snapshot, 0, sizeof(*snapshot));
  beginDebugWrite(snapshot);
  snapshot->magic = DEBUG_SNAPSHOT_MAGIC;
  snapshot->format_version = DEBUG_SNAPSHOT_VERSION;
  snapshot->snapshot_size = sizeof(DebugSnapshot);
  snapshot->boot_session_id = debug_boot_session_id;
  snapshot->cmd9_sequence = ++debug_cmd9_sequence;
  snapshot->target_frequency = frequency;
  snapshot->step1_busy = 0xFF;
  snapshot->step2_busy = 0xFF;
  snapshot->step1_transfer_return = 0xFF;
  snapshot->step2_transfer_return = 0xFF;
  snapshot->reached_flags |= (uint16_t)(1u << DEBUG_CMD9_CALLBACK);
  snapshot->stage_micros[DEBUG_CMD9_CALLBACK] = micros();
  endDebugWrite(snapshot);

  debug_track_notify = false;
}

static void markDebugStage(DebugStage stage)
{
  DebugSnapshot* snapshot = activeDebugSnapshot();

  if (snapshot == nullptr)
    return;

  uint16_t flag = (uint16_t)(1u << stage);

  if ((snapshot->reached_flags & flag) != 0)
    return;

  beginDebugWrite(snapshot);
  snapshot->stage_micros[stage] = micros();
  snapshot->reached_flags |= flag;
  endDebugWrite(snapshot);
}

static DebugSnapshot* selectDebugSnapshotForRead()
{
  if (debug_latest_slot >= DEBUG_SNAPSHOT_SLOT_COUNT)
    return nullptr;

  const uint16_t completed_flag = (uint16_t)(1u << DEBUG_NOTIFY_STEP2_RETURNED);

  // Prefer the newest incomplete CMD9 record so a later successful command
  // does not immediately hide the failed command that this probe targets.
  for (uint8_t offset = 0; offset < DEBUG_SNAPSHOT_SLOT_COUNT; offset++)
  {
    uint8_t slot =
      (uint8_t)((debug_latest_slot + DEBUG_SNAPSHOT_SLOT_COUNT - offset) % DEBUG_SNAPSHOT_SLOT_COUNT);
    DebugSnapshot* snapshot = &debug_snapshots[slot];

    if (snapshot->magic == DEBUG_SNAPSHOT_MAGIC &&
        snapshot->format_version == DEBUG_SNAPSHOT_VERSION &&
        snapshot->snapshot_size == sizeof(DebugSnapshot) &&
        (snapshot->reached_flags & completed_flag) == 0)
      return snapshot;
  }

  return activeDebugSnapshot();
}

static bool copyDebugSnapshotForRead()
{
  DebugSnapshot* snapshot = selectDebugSnapshotForRead();

  if (snapshot == nullptr)
    return false;

  for (uint8_t attempt = 0; attempt < 3; attempt++)
  {
    uint32_t before = snapshot->generation;

    if ((before & 1u) != 0)
      continue;

    memcpy(&debug_response, snapshot, sizeof(debug_response));

    uint32_t after = snapshot->generation;

    if (before == after && (after & 1u) == 0)
      return true;
  }

  return false;
}

extern "C"
{
  bool usbd_edpt_xfer(
    uint8_t rhport,
    uint8_t ep_addr,
    uint8_t *buffer,
    uint16_t total_bytes
  );

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

  if (freq_val > 5000)
  {
    // 200kHz 스페이싱이므로 10이 아니라 20으로 나눕니다.
    channel = (freq_val - 8750) / 20; 
  }
  else
  {
    // 혹시라도 875.0 포맷으로 들어올 경우를 대비해 1이 아닌 2로 나눕니다.
    channel = (freq_val - 875) / 2; 
  }

  rx.setChannel(channel);
}

// ----------------------------------------------------
// 삼성 USB 인터페이스
// ----------------------------------------------------
class SamsungHybridInterface : public Adafruit_USBD_Interface
{
public:

  virtual uint16_t getInterfaceDescriptor(
    uint8_t itfnum,
    uint8_t* buf,
    uint16_t bufsize
  )
  {
    uint8_t desc[] =
    {
      9,
      TUSB_DESC_INTERFACE,
      2,
      0,
      0,
      TUSB_CLASS_VENDOR_SPECIFIC,
      0,
      0,
      0,

      9,
      TUSB_DESC_INTERFACE,
      3,
      0,
      0,
      TUSB_CLASS_VENDOR_SPECIFIC,
      0,
      0,
      0,

      9,
      TUSB_DESC_INTERFACE,
      4,
      0,
      1,
      TUSB_CLASS_VENDOR_SPECIFIC,
      0,
      0,
      0,

      7,
      TUSB_DESC_ENDPOINT,
      0x85,
      TUSB_XFER_INTERRUPT,
      5,
      0,
      4
    };

    if (bufsize < sizeof(desc))
      return 0;

    memcpy(buf, desc, sizeof(desc));

    return sizeof(desc);
  }
};

SamsungHybridInterface samsung_hybrid;

enum BesCmd
{
  SET   = 161,
  GET   = 162,
  QUERY = 163
};

// ----------------------------------------------------
// SETUP
// ----------------------------------------------------
void setup()
{
  debug_boot_session_id = micros();

  USBDevice.setID(0x04E8, 0xA05B);

  USBDevice.setManufacturerDescriptor("Samsung");
  USBDevice.setProductDescriptor("Samsung USB C Earphone");

  USBDevice.addInterface(samsung_hybrid);

  USBDevice.detach();
  delay(100);
  USBDevice.attach();

  // I2C
  Wire.setSDA(SDA_PIN);
  Wire.setSCL(SCL_PIN);

  Wire.begin();

  Wire.setClock(100000);

  // SI4703
  rx.setup(RESET_PIN, DUMMY_INT);

  // 중요
  // 네 환경에서는 0이 정상 동작
  rx.setBand(0);

  // 200kHz spacing
  rx.setSpace(0);

  rx.setMute(true);

  rx.setVolume(0);

  safeTune(current_freq);
}

// ----------------------------------------------------
// LOOP
// ----------------------------------------------------
void loop()
{
#if defined(ARDUINO_ARCH_RP2040)
  TinyUSBDevice.task();
#endif

  // ------------------------------------------------
  // SEEK 시작
  // ------------------------------------------------
  if (pending_cmd == 7 && !waiting_for_seek)
  {
    if (target_val == 1)
    {
      rx.seek(0, 1);
    }
    else
    {
      rx.seek(0, 0);
    }

    seek_start_time = millis();

    waiting_for_seek = true;

    pending_cmd = 0;
  }

      if (waiting_for_seek && millis() - seek_start_time > 60)
      {
          // 기존: uint16_t raw_f = rx.getFrequency();
          uint16_t fake_f = rx.getFrequency(); // 라이브러리가 100kHz로 잘못 계산한 값

          // 가짜 주파수에서 칩셋의 진짜 채널 번호를 역추출
          uint16_t channel = (fake_f - 8750) / 10;

          // 200kHz(한국 규격)를 곱해서 진짜 주파수 복원
          uint16_t real_f = 8750 + (channel * 20);

          // 복원된 진짜 주파수 사용
          current_freq = real_f;

          last_rssi = rx.getRssi();

          last_seek_success = (last_rssi > 15);

          notify_state = 1;

          waiting_for_seek = false;
      }
  

  // ------------------------------------------------
  // DIRECT TUNE
  // ------------------------------------------------
  if (pending_cmd == 9)
  {
    markDebugStage(DEBUG_CMD9_MAIN_LOOP);

    current_freq = target_val;

    markDebugStage(DEBUG_SAFE_TUNE_ENTERED);
    safeTune(current_freq);
    markDebugStage(DEBUG_SAFE_TUNE_RETURNED);

    markDebugStage(DEBUG_RSSI_ENTERED);
    last_rssi = rx.getRssi();
    markDebugStage(DEBUG_RSSI_RETURNED);

    DebugSnapshot* snapshot = activeDebugSnapshot();

    if (snapshot != nullptr)
    {
      beginDebugWrite(snapshot);
      snapshot->snapshot_rssi = last_rssi;
      endDebugWrite(snapshot);
    }

    last_seek_success = true;

    notify_state = 1;
    markDebugStage(DEBUG_NOTIFY_STATE_SET);

    snapshot = activeDebugSnapshot();

    if (snapshot != nullptr)
    {
      beginDebugWrite(snapshot);
      snapshot->notify_state_after_set = notify_state;
      endDebugWrite(snapshot);
    }

    debug_track_notify = true;

    pending_cmd = 0;
  }

  // ------------------------------------------------
  // NOTIFY STEP 1
  // ------------------------------------------------
  if (notify_state == 1)
  {
    bool step1_busy = usbd_edpt_busy(0, 0x85);

    if (debug_track_notify)
    {
      markDebugStage(DEBUG_NOTIFY_STEP1_CHECKED);

      DebugSnapshot* snapshot = activeDebugSnapshot();

      if (snapshot != nullptr && snapshot->step1_busy == 0xFF)
      {
        beginDebugWrite(snapshot);
        snapshot->step1_busy = step1_busy ? 1 : 0;
        endDebugWrite(snapshot);
      }
    }

    if (!step1_busy)
    {
      static uint8_t step1[5] =
      {
        0x01,
        0x00,
        0x08,
        0x00,
        0x00
      };

      if (debug_track_notify)
        markDebugStage(DEBUG_NOTIFY_STEP1_ATTEMPTED);

      bool step1_result = usbd_edpt_xfer(0, 0x85, step1, 5);

      if (debug_track_notify)
      {
        markDebugStage(DEBUG_NOTIFY_STEP1_RETURNED);

        DebugSnapshot* snapshot = activeDebugSnapshot();

        if (snapshot != nullptr)
        {
          beginDebugWrite(snapshot);
          snapshot->step1_transfer_return = step1_result ? 1 : 0;
          endDebugWrite(snapshot);
        }
      }

      notify_state = 2;

      if (debug_track_notify)
      {
        DebugSnapshot* snapshot = activeDebugSnapshot();

        if (snapshot != nullptr)
        {
          beginDebugWrite(snapshot);
          snapshot->notify_state_after_step1 = notify_state;
          endDebugWrite(snapshot);
        }
      }
    }
  }

  // ------------------------------------------------
  // NOTIFY STEP 2
  // ------------------------------------------------
  else if (notify_state == 2)
  {
    bool step2_busy = usbd_edpt_busy(0, 0x85);

    if (debug_track_notify)
    {
      markDebugStage(DEBUG_NOTIFY_STEP2_CHECKED);

      DebugSnapshot* snapshot = activeDebugSnapshot();

      if (snapshot != nullptr && snapshot->step2_busy == 0xFF)
      {
        beginDebugWrite(snapshot);
        snapshot->step2_busy = step2_busy ? 1 : 0;
        endDebugWrite(snapshot);
      }
    }

    if (!step2_busy)
    {
      uint8_t step2[5] =
      {
        0x01,
        0x01,

        (uint8_t)(current_freq & 0xFF),
        (uint8_t)((current_freq >> 8) & 0xFF),

        last_rssi
      };

      if (debug_track_notify)
        markDebugStage(DEBUG_NOTIFY_STEP2_ATTEMPTED);

      bool step2_result = usbd_edpt_xfer(0, 0x85, step2, 5);

      if (debug_track_notify)
      {
        markDebugStage(DEBUG_NOTIFY_STEP2_RETURNED);

        DebugSnapshot* snapshot = activeDebugSnapshot();

        if (snapshot != nullptr)
        {
          beginDebugWrite(snapshot);
          snapshot->step2_transfer_return = step2_result ? 1 : 0;
          endDebugWrite(snapshot);
        }
      }

      notify_state = 0;

      if (debug_track_notify)
      {
        DebugSnapshot* snapshot = activeDebugSnapshot();

        if (snapshot != nullptr)
        {
          beginDebugWrite(snapshot);
          snapshot->notify_state_after_step2 = notify_state;
          endDebugWrite(snapshot);
        }
      }

      debug_track_notify = false;
    }
  }
}

// ----------------------------------------------------
// AUDIO CALLBACK
// ----------------------------------------------------
extern "C"
bool tud_audio_rx_done_cb(
  uint8_t rhport,
  uint16_t n_bytes
)
{
  return true;
}

// ----------------------------------------------------
// USB CONTROL CALLBACK
// ----------------------------------------------------
extern "C"
bool tud_vendor_control_xfer_cb(
  uint8_t rhport,
  uint8_t stage,
  tusb_control_request_t const * request
)
{
  if (stage != CONTROL_STAGE_SETUP)
  {
    return true;
  }

  uint8_t req_type = request->bRequest;

  uint16_t cmd = request->wValue;
  uint16_t val = request->wIndex;

  // Read-only CMD9 debug snapshot request:
  // bmRequestType=0xC0, bRequest=0xD9, wValue=0x464D, wIndex=1, wLength=88
  if (request->bmRequestType == 0xC0 &&
      request->bRequest == DEBUG_SNAPSHOT_REQUEST &&
      request->wValue == DEBUG_SNAPSHOT_VALUE &&
      request->wIndex == DEBUG_SNAPSHOT_VERSION &&
      request->wLength == sizeof(DebugSnapshot))
  {
    if (!copyDebugSnapshotForRead())
      return false;

    return tud_control_xfer(
      rhport,
      request,
      (uint8_t*)&debug_response,
      sizeof(debug_response)
    );
  }

  // ------------------------------------------------
  // SET
  // ------------------------------------------------
  if (req_type == BesCmd::SET)
  {
    switch (cmd)
    {
      // RADIO ON/OFF
      case 0:

        if (val == 1)
        {
          rx.setMute(false);

          rx.setVolume(current_vol);
        }
        else
        {
          rx.setMute(true);
        }

        break;

      // MUTE
      case 4:

        if (val == 1)
        {
          rx.setMute(true);
        }
        else
        {
          rx.setMute(false);

          rx.setVolume(current_vol);
        }

        break;

      // VOLUME
      case 5:

        current_vol = val;

        rx.setVolume(current_vol);

        break;

      // SEEK
      case 7:

        target_val = val;

        pending_cmd = 7;

        break;

      // DIRECT TUNE
      case 9:

        target_val = val;

        pending_cmd = 9;

        startCmd9DebugSnapshot(val);

        break;
    }

    // ACK
    if (request->wLength == 0)
    {
      return tud_control_status(rhport, request);
    }
    else
    {
      static uint8_t ack_buf[64] = {1};

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
  else if (
    req_type == BesCmd::GET ||
    req_type == BesCmd::QUERY
  )
  {
    // GET
    if (req_type == BesCmd::GET)
    {
      static uint8_t res_get[64] = {0};

      uint16_t data = 1;

      if (cmd == 8)
      {
        data = current_vol;
      }
      else if (cmd == 13)
      {
        data = current_freq;
      }
      else if (cmd == 17) {
      data = 0;
      }

      memcpy(res_get, &data, 2);

      return tud_control_xfer(
        rhport,
        request,
        res_get,
        request->wLength
      );
    }

    // QUERY
    static uint8_t res_query[64] = {0};

    res_query[0] =
      last_seek_success ? 1 : 0;

    res_query[1] = 1;

    res_query[2] =
      current_freq & 0xFF;

    res_query[3] =
      (current_freq >> 8) & 0xFF;

    res_query[4] = last_rssi;

    return tud_control_xfer(
      rhport,
      request,
      res_query,
      request->wLength
    );
  }

  return false;
}
