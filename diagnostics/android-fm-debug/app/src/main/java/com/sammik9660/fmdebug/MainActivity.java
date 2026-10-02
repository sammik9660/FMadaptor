package com.sammik9660.fmdebug;

import android.app.Activity;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.hardware.usb.UsbConstants;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbManager;
import android.os.Build;
import android.os.Bundle;
import android.util.Log;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.Locale;

public final class MainActivity extends Activity {
    private static final String TAG = "FMDBG";
    private static final String ACTION_USB_PERMISSION =
            "com.sammik9660.fmdebug.USB_PERMISSION";
    private static final int VID = 0x04E8;
    private static final int PID = 0xA05B;
    private static final int REQUEST_TYPE =
            UsbConstants.USB_DIR_IN | UsbConstants.USB_TYPE_VENDOR;
    private static final int REQUEST = 0xD9;
    private static final int VALUE = 0x464D;
    private static final int FORMAT_VERSION = 4;
    private static final int SNAPSHOT_SIZE = 96;
    private static final int TRANSFER_TIMEOUT_MS = 1000;
    private static final int MAGIC = 0x47424446;

    private static final String[] STAGE_NAMES = {
            "CMD9_CALLBACK", "CMD9_MAIN_LOOP", "SAFE_TUNE_ENTERED",
            "SAFE_TUNE_RETURNED", "RSSI_ENTERED", "RSSI_RETURNED",
            "NOTIFY_STATE_SET", "NOTIFY_STEP1_CHECKED",
            "NOTIFY_STEP1_ATTEMPTED", "NOTIFY_STEP1_RETURNED",
            "NOTIFY_STEP2_CHECKED", "NOTIFY_STEP2_ATTEMPTED",
            "NOTIFY_STEP2_RETURNED"
    };

    private static final String[] TUNE_RESULT_NAMES = {
            "TUNE_NOT_RUN", "TUNE_OK", "TUNE_TIMEOUT_WAIT_STC_SET",
            "TUNE_TIMEOUT_WAIT_STC_CLEAR", "TUNE_I2C_ERROR"
    };

    private static final String[] BOOT_STAGE_NAMES = {
            "BOOT_NOT_STARTED", "SETUP_STARTED", "RX_SETUP_ENTERED",
            "RX_SETUP_RETURNED", "INITIAL_TUNE_ENTERED",
            "INITIAL_TUNE_RETURNED", "SETUP_COMPLETE"
    };

    private UsbManager usbManager;
    private TextView deviceStatus;
    private TextView permissionStatus;
    private TextView resultView;

    private final BroadcastReceiver permissionReceiver = new BroadcastReceiver() {
        @Override public void onReceive(Context context, Intent intent) {
            if (!ACTION_USB_PERMISSION.equals(intent.getAction())) return;
            UsbDevice device = getUsbDevice(intent);
            boolean granted = intent.getBooleanExtra(
                    UsbManager.EXTRA_PERMISSION_GRANTED, false);
            updateStatus();
            if (granted && device != null) readSnapshot(device);
            else showResult("USB permission denied");
        }
    };

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        usbManager = (UsbManager) getSystemService(Context.USB_SERVICE);
        IntentFilter filter = new IntentFilter(ACTION_USB_PERMISSION);
        if (Build.VERSION.SDK_INT >= 33) {
            registerReceiver(permissionReceiver, filter, Context.RECEIVER_NOT_EXPORTED);
        } else {
            registerReceiver(permissionReceiver, filter);
        }
        buildUi();
        updateStatus();
    }

    @Override protected void onResume() {
        super.onResume();
        updateStatus();
    }

    @Override protected void onDestroy() {
        unregisterReceiver(permissionReceiver);
        super.onDestroy();
    }

    private void buildUi() {
        int padding = (int) (16 * getResources().getDisplayMetrics().density);
        LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setPadding(padding, padding, padding, padding);
        deviceStatus = addText(content, "DEVICE STATUS: checking");
        permissionStatus = addText(content, "USB PERMISSION STATUS: checking");
        Button readButton = new Button(this);
        readButton.setText("READ SNAPSHOT");
        readButton.setOnClickListener(view -> requestOrRead());
        content.addView(readButton, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));
        resultView = addText(content, "RESULT\nNo snapshot read yet.");
        resultView.setTextIsSelectable(true);
        ScrollView scroll = new ScrollView(this);
        scroll.addView(content);
        setContentView(scroll);
    }

    private TextView addText(LinearLayout parent, String text) {
        TextView view = new TextView(this);
        view.setText(text);
        view.setTextSize(16);
        view.setPadding(0, 0, 0, 16);
        parent.addView(view, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));
        return view;
    }

    private UsbDevice findTargetDevice() {
        for (UsbDevice device : usbManager.getDeviceList().values()) {
            if (device.getVendorId() == VID && device.getProductId() == PID) return device;
        }
        return null;
    }

    private void updateStatus() {
        if (deviceStatus == null) return;
        UsbDevice device = findTargetDevice();
        deviceStatus.setText(device == null
                ? "DEVICE STATUS: 04E8:A05B not found"
                : "DEVICE STATUS: 04E8:A05B connected");
        permissionStatus.setText(device == null
                ? "USB PERMISSION STATUS: unavailable"
                : "USB PERMISSION STATUS: "
                + (usbManager.hasPermission(device) ? "granted" : "not granted"));
    }

    private void requestOrRead() {
        UsbDevice device = findTargetDevice();
        if (device == null) {
            showResult("Target device 04E8:A05B not found");
            return;
        }
        if (!usbManager.hasPermission(device)) {
            int flags = Build.VERSION.SDK_INT >= Build.VERSION_CODES.S
                    ? PendingIntent.FLAG_MUTABLE : 0;
            PendingIntent intent = PendingIntent.getBroadcast(this, 0,
                    new Intent(ACTION_USB_PERMISSION).setPackage(getPackageName()), flags);
            usbManager.requestPermission(device, intent);
            showResult("Waiting for USB permission");
            return;
        }
        readSnapshot(device);
    }

    private void readSnapshot(UsbDevice device) {
        showResult("Reading snapshot...");
        new Thread(() -> {
            UsbDeviceConnection connection = usbManager.openDevice(device);
            if (connection == null) {
                showResult("openDevice() failed");
                return;
            }
            byte[] response = new byte[SNAPSHOT_SIZE];
            int transferred;
            try {
                // No interface claim: endpoint 0 control transfer only.
                transferred = connection.controlTransfer(
                        REQUEST_TYPE, REQUEST, VALUE, FORMAT_VERSION,
                        response, response.length, TRANSFER_TIMEOUT_MS);
            } finally {
                connection.close();
            }
            if (transferred != SNAPSHOT_SIZE) {
                showResult("controlTransfer returned " + transferred
                        + "; expected " + SNAPSHOT_SIZE);
                return;
            }
            try {
                showResult(formatSnapshot(response));
            } catch (IllegalArgumentException error) {
                showResult("Invalid snapshot: " + error.getMessage());
            }
        }, "FMDBG-reader").start();
    }

    private String formatSnapshot(byte[] bytes) {
        ByteBuffer data = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN);
        int magic = data.getInt();
        int version = Short.toUnsignedInt(data.getShort());
        int size = Short.toUnsignedInt(data.getShort());
        if (magic != MAGIC) throw new IllegalArgumentException(String.format(
                Locale.US, "bad magic 0x%08X", magic));
        if (version != FORMAT_VERSION)
            throw new IllegalArgumentException("unsupported version " + version);
        if (size != SNAPSHOT_SIZE)
            throw new IllegalArgumentException("unexpected size " + size);

        long bootId = Integer.toUnsignedLong(data.getInt());
        long generation = Integer.toUnsignedLong(data.getInt());
        long sequence = Integer.toUnsignedLong(data.getInt());
        int target = Short.toUnsignedInt(data.getShort());
        int flags = Short.toUnsignedInt(data.getShort());
        long[] timestamps = new long[STAGE_NAMES.length];
        for (int i = 0; i < timestamps.length; i++) {
            timestamps[i] = Integer.toUnsignedLong(data.getInt());
        }
        int step1Busy = Byte.toUnsignedInt(data.get());
        int step2Busy = Byte.toUnsignedInt(data.get());
        int step1Return = Byte.toUnsignedInt(data.get());
        int step2Return = Byte.toUnsignedInt(data.get());
        int rssi = Byte.toUnsignedInt(data.get());
        int stateSet = Byte.toUnsignedInt(data.get());
        int stateAfter1 = Byte.toUnsignedInt(data.get());
        int stateAfter2 = Byte.toUnsignedInt(data.get());
        int tuneResult = Byte.toUnsignedInt(data.get());
        int stcSetTimeout = Byte.toUnsignedInt(data.get());
        int stcClearTimeout = Byte.toUnsignedInt(data.get());
        int lastStc = Byte.toUnsignedInt(data.get());
        int recoveryAttempted = Byte.toUnsignedInt(data.get());
        long tuneFailureMicros = Integer.toUnsignedLong(data.getInt());
        int bootStage = Byte.toUnsignedInt(data.get());
        int initialTuneResult = Byte.toUnsignedInt(data.get());
        int initialCleanupWrite = Byte.toUnsignedInt(data.get());

        StringBuilder text = new StringBuilder("BOOT STAGE\n");
        text.append(formatBootStage(bootStage)).append("\n\nINITIAL TUNE\n")
                .append("initial_tune_result=").append(formatTuneResult(initialTuneResult)).append('\n')
                .append("initial_cleanup_write_ok=")
                .append(initialCleanupWrite == 0xFF ? "not attempted" : formatTriState(initialCleanupWrite))
                .append("\n\nCMD9 DEBUG\n");
        text.append("magic=FDBG version=").append(version)
                .append(" size=").append(size).append('\n')
                .append("boot_session=").append(bootId)
                .append(" generation=").append(generation)
                .append(" cmd9_sequence=").append(sequence).append('\n')
                .append("target_frequency=").append(target)
                .append(" flags=0x")
                .append(String.format(Locale.US, "%04X", flags)).append('\n');
        for (int i = 0; i < STAGE_NAMES.length; i++) {
            boolean reached = (flags & (1 << i)) != 0;
            text.append(STAGE_NAMES[i]).append('=')
                    .append(reached ? "reached @ " + timestamps[i] + " us" : "not reached")
                    .append('\n');
        }
        text.append("step1_busy=").append(formatTriState(step1Busy)).append('\n')
                .append("step2_busy=").append(formatTriState(step2Busy)).append('\n')
                .append("step1_transfer_return=").append(formatTriState(step1Return)).append('\n')
                .append("step2_transfer_return=").append(formatTriState(step2Return)).append('\n')
                .append("last_rssi=").append(rssi).append('\n')
                .append("notify_state_after_set=").append(stateSet).append('\n')
                .append("notify_state_after_step1=").append(stateAfter1).append('\n')
                .append("notify_state_after_step2=").append(stateAfter2).append('\n')
                .append("tune_result=").append(formatTuneResult(tuneResult)).append('\n')
                .append("stc_set_timeout=").append(stcSetTimeout != 0).append('\n')
                .append("stc_clear_timeout=").append(stcClearTimeout != 0).append('\n')
                .append("last_stc=").append(formatLastStc(lastStc)).append('\n')
                .append("tune_failure_micros=").append(tuneFailureMicros).append('\n')
                .append("recovery_attempted=").append(recoveryAttempted != 0);
        return text.toString();
    }

    private String formatTuneResult(int value) {
        if (value >= 0 && value < TUNE_RESULT_NAMES.length) return TUNE_RESULT_NAMES[value];
        return "UNKNOWN(" + value + ")";
    }

    private String formatBootStage(int value) {
        if (value >= 0 && value < BOOT_STAGE_NAMES.length) return BOOT_STAGE_NAMES[value];
        return "UNKNOWN(" + value + ")";
    }

    private String formatLastStc(int value) {
        if (value == 0xFF) return "not observed";
        return Integer.toString(value);
    }

    private String formatTriState(int value) {
        if (value == 0xFF) return "not observed";
        return value == 0 ? "false" : "true";
    }

    private void showResult(String text) {
        Log.i(TAG, text.replace('\n', ' '));
        runOnUiThread(() -> {
            resultView.setText(text);
            updateStatus();
        });
    }

    @SuppressWarnings("deprecation")
    private UsbDevice getUsbDevice(Intent intent) {
        if (Build.VERSION.SDK_INT >= 33) {
            return intent.getParcelableExtra(UsbManager.EXTRA_DEVICE, UsbDevice.class);
        }
        return intent.getParcelableExtra(UsbManager.EXTRA_DEVICE);
    }
}
