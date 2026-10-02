#include <Adafruit_TinyUSB.h>
#include <Wire.h>
#include <SI470X.h>
#include <stddef.h>

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
static const uint16_t DEBUG_SNAPSHOT_VERSION = 4;
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

enum TuneResult : uint8_t
{
  TUNE_NOT_RUN = 0,
  TUNE_OK,
  TUNE_TIMEOUT_WAIT_STC_SET,
  TUNE_TIMEOUT_WAIT_STC_CLEAR,
  TUNE_I2C_ERROR
};

enum BootStage : uint8_t
{
  BOOT_NOT_STARTED = 0,
  SETUP_STARTED,
  RX_SETUP_ENTERED,
  RX_SETUP_RETURNED,
  INITIAL_TUNE_ENTERED,
  INITIAL_TUNE_RETURNED,
  SETUP_COMPLETE
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
  uint8_t tune_result;
  uint8_t stc_set_timeout;
  uint8_t stc_clear_timeout;
  uint8_t last_stc;
  uint8_t recovery_attempted;
  uint32_t tune_failure_micros;
  uint8_t boot_stage;
  uint8_t initial_tune_result;
  uint8_t initial_cleanup_write_ok;
};
#pragma pack(pop)

static_assert(sizeof(DebugSnapshot) == 96, "DebugSnapshot wire format changed");
static_assert(offsetof(DebugSnapshot, stage_micros) == 24, "Stage timestamp offset changed");
static_assert(offsetof(DebugSnapshot, tune_result) == 84, "CMD9 result offset changed");
static_assert(offsetof(DebugSnapshot, tune_failure_micros) == 89, "Failure timestamp offset changed");
static_assert(offsetof(DebugSnapshot, boot_stage) == 93, "Boot stage offset changed");
static_assert(offsetof(DebugSnapshot, initial_tune_result) == 94, "Initial result offset changed");
static_assert(offsetof(DebugSnapshot, initial_cleanup_write_ok) == 95, "Cleanup result offset changed");

static DebugSnapshot debug_snapshots[DEBUG_SNAPSHOT_SLOT_COUNT];
static DebugSnapshot debug_response;
static uint32_t debug_boot_session_id = 0;
static uint32_t debug_generation = 0;
static uint32_t debug_cmd9_sequence = 0;
static uint8_t debug_latest_slot = 0xFF;
static bool debug_track_notify = false;
static volatile uint8_t debug_boot_stage = BOOT_NOT_STARTED;
static volatile uint8_t debug_initial_tune_result = TUNE_NOT_RUN;
// 0xFF: not attempted; 0: write failed; 1: write ACKed (not physical mute verification).
static volatile uint8_t debug_initial_cleanup_write_ok = 0xFF;

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
  snapshot->tune_result = TUNE_NOT_RUN;
  snapshot->last_stc = 0xFF;
  snapshot->recovery_attempted = 0;
  snapshot->boot_stage = debug_boot_stage;
  snapshot->initial_tune_result = debug_initial_tune_result;
  snapshot->initial_cleanup_write_ok = debug_initial_cleanup_write_ok;
  snapshot->reached_flags |= (uint16_t)(1u << DEBUG_CMD9_CALLBACK);
  snapshot->stage_micros[DEBUG_CMD9_CALLBACK] = micros();
  endDebugWrite(snapshot);

  debug_track_notify = false;
}

static void recordTuneResult(TuneResult result, uint8_t last_stc)
{
  DebugSnapshot* snapshot = activeDebugSnapshot();

  if (snapshot == nullptr)
    return;

  beginDebugWrite(snapshot);
  snapshot->tune_result = result;
  snapshot->stc_set_timeout = (result == TUNE_TIMEOUT_WAIT_STC_SET) ? 1 : 0;
  snapshot->stc_clear_timeout = (result == TUNE_TIMEOUT_WAIT_STC_CLEAR) ? 1 : 0;
  snapshot->last_stc = last_stc;
  snapshot->recovery_attempted = 0;

  if (result != TUNE_OK)
    snapshot->tune_failure_micros = micros();

  endDebugWrite(snapshot);
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
  {
    memset(&debug_response, 0, sizeof(debug_response));
    debug_response.magic = DEBUG_SNAPSHOT_MAGIC;
    debug_response.format_version = DEBUG_SNAPSHOT_VERSION;
    debug_response.snapshot_size = sizeof(DebugSnapshot);
    debug_response.boot_session_id = debug_boot_session_id;
    debug_response.generation = debug_generation;
    debug_response.step1_busy = 0xFF;
    debug_response.step2_busy = 0xFF;
    debug_response.step1_transfer_return = 0xFF;
    debug_response.step2_transfer_return = 0xFF;
    debug_response.tune_result = TUNE_NOT_RUN;
    debug_response.last_stc = 0xFF;
    debug_response.recovery_attempted = 0;
    debug_response.boot_stage = debug_boot_stage;
    debug_response.initial_tune_result = debug_initial_tune_result;
    debug_response.initial_cleanup_write_ok = debug_initial_cleanup_write_ok;
    return true;
  }

  for (uint8_t attempt = 0; attempt < 3; attempt++)
  {
    uint32_t before = snapshot->generation;

    if ((before & 1u) != 0)
      continue;

    memcpy(&debug_response, snapshot, sizeof(debug_response));

    uint32_t after = snapshot->generation;

    if (before == after && (after & 1u) == 0)
    {
      debug_response.boot_stage = debug_boot_stage;
      debug_response.initial_tune_result = debug_initial_tune_result;
      debug_response.initial_cleanup_write_ok = debug_initial_cleanup_write_ok;
      return true;
    }
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

static const uint8_t SI470X_I2C_ADDRESS = 0x10;
static const uint8_t SI470X_REG_POWERCFG = 0x02;
static const uint8_t SI470X_REG_CHANNEL = 0x03;
static const uint8_t SI470X_REG_STATUSRSSI = 0x0A;
static const uint16_t SI470X_SEEK_BIT = 0x0100;
static const uint16_t SI470X_DMUTE_BIT = 0x4000;
static const uint16_t SI470X_CHANNEL_MASK = 0x03FF;
static const uint16_t SI470X_TUNE_BIT = 0x8000;
static const uint16_t SI470X_STC_BIT = 0x4000;
static const uint32_t CMD9_STC_STAGE_TIMEOUT_US = 250000;
static const uint32_t CMD9_I2C_TIMEOUT_MS = 25;

static bool writeSi470xControlRegisters()
{
  Wire.beginTransmission(SI470X_I2C_ADDRESS);

  for (uint8_t reg = 0x02; reg <= 0x07; reg++)
  {
    uint16_t value = rx.getShadownRegister(reg);

    if (Wire.write((uint8_t)(value >> 8)) != 1 ||
        Wire.write((uint8_t)(value & 0xFF)) != 1)
    {
      Wire.endTransmission();
      return false;
    }
  }

  return Wire.endTransmission() == 0;
}

static bool readSi470xStatus(uint8_t* stc)
{
  size_t received = Wire.requestFrom(SI470X_I2C_ADDRESS, (size_t)2);
  delayMicroseconds(300);

  if (received != 2 || Wire.available() < 2)
  {
    while (Wire.available() > 0)
      Wire.read();

    return false;
  }

  int high_byte = Wire.read();
  int low_byte = Wire.read();

  if (high_byte < 0 || low_byte < 0)
    return false;

  uint16_t status = ((uint16_t)high_byte << 8) | (uint8_t)low_byte;
  rx.setShadownRegister(SI470X_REG_STATUSRSSI, status);
  *stc = (status & SI470X_STC_BIT) ? 1 : 0;
  return true;
}

static bool readAllSi470xRegisters()
{
  uint16_t values[16];
  size_t received = Wire.requestFrom(SI470X_I2C_ADDRESS, (size_t)32);
  delayMicroseconds(300);

  if (received != 32 || Wire.available() < 32)
  {
    while (Wire.available() > 0)
      Wire.read();

    return false;
  }

  for (uint8_t offset = 0; offset < 16; offset++)
  {
    int high_byte = Wire.read();
    int low_byte = Wire.read();

    if (high_byte < 0 || low_byte < 0)
      return false;

    values[offset] = ((uint16_t)high_byte << 8) | (uint8_t)low_byte;
  }

  for (uint8_t offset = 0; offset < 16; offset++)
  {
    uint8_t reg = (offset < 6) ? (uint8_t)(0x0A + offset) : (uint8_t)(offset - 6);
    rx.setShadownRegister(reg, values[offset]);
  }

  return true;
}

static TuneResult finishBoundedTune(TuneResult result, uint32_t previous_i2c_timeout)
{
  Wire.setTimeout(previous_i2c_timeout, false);
  return result;
}

static TuneResult boundedTune(uint16_t freq_val, uint8_t* last_stc)
{
  uint16_t channel;

  if (freq_val > 5000)
    channel = (freq_val - 8750) / 20;
  else
    channel = (freq_val - 875) / 2;

  *last_stc = 0xFF;

  uint32_t previous_i2c_timeout = Wire.getTimeout();
  Wire.setTimeout(CMD9_I2C_TIMEOUT_MS, false);

  uint16_t channel_register = rx.getShadownRegister(SI470X_REG_CHANNEL);
  channel_register &= (uint16_t)~SI470X_CHANNEL_MASK;
  channel_register |= channel & SI470X_CHANNEL_MASK;
  channel_register |= SI470X_TUNE_BIT;
  rx.setShadownRegister(SI470X_REG_CHANNEL, channel_register);

  if (!writeSi470xControlRegisters())
    return finishBoundedTune(TUNE_I2C_ERROR, previous_i2c_timeout);

  delayMicroseconds(60000);

  uint32_t stage_started = micros();

  while (true)
  {
    if (!readSi470xStatus(last_stc))
      return finishBoundedTune(TUNE_I2C_ERROR, previous_i2c_timeout);

    if (*last_stc == 1)
      break;

    if ((uint32_t)(micros() - stage_started) >= CMD9_STC_STAGE_TIMEOUT_US)
    {
      channel_register &= (uint16_t)~SI470X_TUNE_BIT;
      rx.setShadownRegister(SI470X_REG_CHANNEL, channel_register);
      writeSi470xControlRegisters();
      return finishBoundedTune(TUNE_TIMEOUT_WAIT_STC_SET, previous_i2c_timeout);
    }
  }

  if (!readAllSi470xRegisters())
    return finishBoundedTune(TUNE_I2C_ERROR, previous_i2c_timeout);

  uint16_t power_register = rx.getShadownRegister(SI470X_REG_POWERCFG);
  power_register &= (uint16_t)~SI470X_SEEK_BIT;
  rx.setShadownRegister(SI470X_REG_POWERCFG, power_register);

  channel_register = rx.getShadownRegister(SI470X_REG_CHANNEL);
  channel_register &= (uint16_t)~SI470X_TUNE_BIT;
  rx.setShadownRegister(SI470X_REG_CHANNEL, channel_register);

  if (!writeSi470xControlRegisters())
    return finishBoundedTune(TUNE_I2C_ERROR, previous_i2c_timeout);

  stage_started = micros();

  while (true)
  {
    if (!readSi470xStatus(last_stc))
      return finishBoundedTune(TUNE_I2C_ERROR, previous_i2c_timeout);

    if (*last_stc == 0)
      return finishBoundedTune(TUNE_OK, previous_i2c_timeout);

    if ((uint32_t)(micros() - stage_started) >= CMD9_STC_STAGE_TIMEOUT_US)
      return finishBoundedTune(TUNE_TIMEOUT_WAIT_STC_CLEAR, previous_i2c_timeout);
  }
}

// Initial tune uses the same bounded operation as CMD9, without CMD9 side effects.
static TuneResult boundedInitialTune(uint16_t freq_val)
{
  uint8_t last_stc = 0xFF;
  TuneResult result = boundedTune(freq_val, &last_stc);

  if (result != TUNE_OK)
  {
    // One best-effort cleanup write; no tune retry, reset, or recovery loop.
    uint32_t previous_i2c_timeout = Wire.getTimeout();
    Wire.setTimeout(CMD9_I2C_TIMEOUT_MS, false);

    uint16_t channel_register = rx.getShadownRegister(SI470X_REG_CHANNEL);
    channel_register &= (uint16_t)~SI470X_TUNE_BIT;
    rx.setShadownRegister(SI470X_REG_CHANNEL, channel_register);

    uint16_t power_register = rx.getShadownRegister(SI470X_REG_POWERCFG);
    power_register &= (uint16_t)~SI470X_DMUTE_BIT;
    rx.setShadownRegister(SI470X_REG_POWERCFG, power_register);

    debug_initial_cleanup_write_ok = writeSi470xControlRegisters() ? 1 : 0;
    Wire.setTimeout(previous_i2c_timeout, false);
  }

  return result;
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
  debug_boot_stage = SETUP_STARTED;

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
  debug_boot_stage = RX_SETUP_ENTERED;
  rx.setup(RESET_PIN, DUMMY_INT);
  debug_boot_stage = RX_SETUP_RETURNED;

  // 중요
  // 네 환경에서는 0이 정상 동작
  rx.setBand(0);

  // 200kHz spacing
  rx.setSpace(0);

  rx.setMute(true);

  rx.setVolume(0);

  debug_boot_stage = INITIAL_TUNE_ENTERED;
  debug_initial_tune_result = boundedInitialTune(current_freq);
  debug_boot_stage = INITIAL_TUNE_RETURNED;
  debug_boot_stage = SETUP_COMPLETE;
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

    markDebugStage(DEBUG_SAFE_TUNE_ENTERED);
    uint8_t tune_last_stc = 0xFF;
    TuneResult tune_result = boundedTune(target_val, &tune_last_stc);
    markDebugStage(DEBUG_SAFE_TUNE_RETURNED);
    recordTuneResult(tune_result, tune_last_stc);

    if (tune_result == TUNE_OK)
    {
      current_freq = target_val;

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
    }
    else
    {
      last_seek_success = false;
      notify_state = 0;
      debug_track_notify = false;
    }

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
  // bmRequestType=0xC0, bRequest=0xD9, wValue=0x464D, wIndex=4, wLength=96
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
