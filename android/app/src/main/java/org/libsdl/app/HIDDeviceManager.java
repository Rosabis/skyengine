package org.libsdl.app;

import android.content.Context;
import android.hardware.usb.UsbDevice;

/**
 * No-op stand-in for SDL's HID/gamepad manager.
 * vmrp only needs touch + on-screen keys; USB/Bluetooth controllers are not used.
 */
public class HIDDeviceManager {
    private static HIDDeviceManager sInstance;
    private static int sRefCount;

    public static HIDDeviceManager acquire(Context context) {
        if (sRefCount == 0) {
            sInstance = new HIDDeviceManager();
        }
        sRefCount++;
        return sInstance;
    }

    public static void release(HIDDeviceManager manager) {
        if (manager == null || manager != sInstance) {
            return;
        }
        sRefCount--;
        if (sRefCount <= 0) {
            sRefCount = 0;
            sInstance = null;
        }
    }

    private HIDDeviceManager() {
    }

    public boolean isSteamController(UsbDevice usbDevice) {
        return false;
    }

    public void setFrozen(boolean frozen) {
    }
}
