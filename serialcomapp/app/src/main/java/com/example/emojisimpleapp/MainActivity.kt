package com.example.emojisimpleapp

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbManager
import android.os.Build
import android.os.Bundle
import android.util.Log
import android.widget.Button
import android.widget.TextView
import android.widget.Toast
import android.widget.ToggleButton
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import com.hoho.android.usbserial.driver.CdcAcmSerialDriver
import com.hoho.android.usbserial.driver.Ch34xSerialDriver
import com.hoho.android.usbserial.driver.Cp21xxSerialDriver
import com.hoho.android.usbserial.driver.FtdiSerialDriver
import com.hoho.android.usbserial.driver.ProbeTable
import com.hoho.android.usbserial.driver.UsbSerialDriver
import com.hoho.android.usbserial.driver.UsbSerialPort
import com.hoho.android.usbserial.driver.UsbSerialProber

class MainActivity : AppCompatActivity() {

    private val TAG = "DualESP32"
    private val ACTION_USB_PERMISSION = "com.example.emojisimpleapp.USB_PERMISSION"
    private lateinit var usbManager: UsbManager
    private var port1: UsbSerialPort? = null
    private var port2: UsbSerialPort? = null
    private lateinit var statusText: TextView
    private var permissionRequested = false
    private var device1Enabled = true
    private var device2Enabled = true

    private val usbPermissionReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            try {
                if (intent.action == ACTION_USB_PERMISSION) {
                    synchronized(this) {
                        permissionRequested = false
                        val granted = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)
                        if (granted) {
                            Log.d(TAG, "Permission granted")
                            statusText.postDelayed({ requestPermissionsAndConnect() }, 300)
                        } else {
                            toast("Permission denied. Tap Reconnect.")
                            updateStatus()
                        }
                    }
                }
            } catch (e: Exception) {
                Log.e(TAG, "Permission error: ${e.message}")
                permissionRequested = false
            }
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        try {
            setContentView(R.layout.activity_main)
            usbManager = getSystemService(Context.USB_SERVICE) as UsbManager
            statusText = findViewById(R.id.statusText)

            ContextCompat.registerReceiver(
                this, usbPermissionReceiver, IntentFilter(ACTION_USB_PERMISSION),
                ContextCompat.RECEIVER_NOT_EXPORTED
            )

            findViewById<ToggleButton>(R.id.toggleDevice1).setOnCheckedChangeListener { _, isChecked ->
                device1Enabled = isChecked; updateStatus()
            }
            findViewById<ToggleButton>(R.id.toggleDevice2).setOnCheckedChangeListener { _, isChecked ->
                device2Enabled = isChecked; updateStatus()
            }

            findViewById<Button>(R.id.btnReconnect).setOnClickListener {
                try { port1?.close(); port2?.close() } catch (_: Exception) {}
                port1 = null; port2 = null; permissionRequested = false
                updateStatus(); toast("Reconnecting...")
                statusText.postDelayed({ requestPermissionsAndConnect() }, 500)
            }
            findViewById<Button>(R.id.btn1).setOnClickListener { sendToDevices("1") }
            findViewById<Button>(R.id.btn2).setOnClickListener { sendToDevices("2") }
            findViewById<Button>(R.id.btn3).setOnClickListener { sendToDevices("3") }
            findViewById<Button>(R.id.btn4).setOnClickListener { sendToDevices("4") }

            statusText.postDelayed({ if (!isFinishing) requestPermissionsAndConnect() }, 1000)
        } catch (e: Exception) {
            Toast.makeText(this, "Init error: ${e.message}", Toast.LENGTH_LONG).show()
            finish()
        }
    }

    /**
     * Request permissions for all USB devices (except hubs), then connect.
     * NOTE: deviceClass=0 is common for composite devices including ESP32 — do NOT filter it.
     */
    private fun requestPermissionsAndConnect() {
        if (permissionRequested) return

        // Log every USB device on the bus for debugging
        for (dev in usbManager.deviceList.values) {
            Log.d(TAG, "USB bus: id=${dev.deviceId} VID=0x${dev.vendorId.toString(16)} " +
                "PID=0x${dev.productId.toString(16)} class=${dev.deviceClass} " +
                "name=${dev.productName ?: "?"} perm=${usbManager.hasPermission(dev)}")
        }

        // Only skip USB hubs (class 9). Class 0 = composite devices (ESP32 uses this!)
        val usbDevices = usbManager.deviceList.values.filter { it.deviceClass != 9 }

        if (usbDevices.isEmpty()) {
            toast("No USB devices found")
            updateStatus(); return
        }

        // Request permission one at a time
        for (device in usbDevices) {
            if (!usbManager.hasPermission(device)) {
                permissionRequested = true
                Log.d(TAG, "Requesting permission for VID=0x${device.vendorId.toString(16)}")
                val intent = Intent(ACTION_USB_PERMISSION).apply { setPackage(packageName) }
                val flags = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S)
                    PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
                else PendingIntent.FLAG_UPDATE_CURRENT
                usbManager.requestPermission(device, PendingIntent.getBroadcast(this, 0, intent, flags))
                return
            }
        }

        // All permitted — try to open
        openSerialPorts()
    }

    /**
     * Open serial ports.
     * Step 1: Use default prober across ALL permitted devices (safe — just checks VID/PID tables)
     * Step 2: Brute-force remaining devices, but SKIP audio(1), HID(3), storage(8), hub(9), wireless(0xE0)
     *         to avoid fighting with kernel drivers (4G modem, audio devices, etc.)
     */
    private fun openSerialPorts() {
        val openedDeviceIds = mutableSetOf<Int>()
        var count = 0

        // Step 1: Default prober — catches known VID/PIDs (Espressif, CH340, CP210x, FTDI, etc.)
        val defaultDrivers = UsbSerialProber.getDefaultProber().findAllDrivers(usbManager)
        Log.d(TAG, "Default prober found ${defaultDrivers.size} driver(s)")
        for (driver in defaultDrivers) {
            if (count >= 2) break
            if (!usbManager.hasPermission(driver.device)) {
                Log.w(TAG, "No permission for VID=0x${driver.device.vendorId.toString(16)}"); continue
            }
            val conn = usbManager.openDevice(driver.device)
            if (conn == null) { Log.e(TAG, "openDevice returned null"); continue }
            try {
                val sp = driver.ports[0]
                sp.open(conn)
                sp.setParameters(115200, 8, UsbSerialPort.STOPBITS_1, UsbSerialPort.PARITY_NONE)
                assignPort(count, sp)
                openedDeviceIds.add(driver.device.deviceId)
                count++
                Log.d(TAG, "✓ Default prober opened #$count: VID=0x${driver.device.vendorId.toString(16)} ${driver.javaClass.simpleName}")
            } catch (e: Exception) {
                Log.e(TAG, "Default prober open failed: ${e.message}")
                conn.close()
            }
        }

        // Step 2: Brute-force remaining devices with a fresh connection per driver attempt.
        // Skip device classes that are definitely NOT serial ports to avoid kernel driver conflicts.
        val skipBruteForce = setOf(1, 3, 8, 9, 0xE0) // audio, HID, mass-storage, hub, wireless
        val driverTypes = listOf(
            CdcAcmSerialDriver::class.java,
            Ch34xSerialDriver::class.java,
            Cp21xxSerialDriver::class.java,
            FtdiSerialDriver::class.java
        )

        for (device in usbManager.deviceList.values) {
            if (count >= 2) break
            if (device.deviceId in openedDeviceIds) continue
            if (!usbManager.hasPermission(device)) continue
            if (device.deviceClass in skipBruteForce) {
                Log.d(TAG, "Skipping brute-force for class=${device.deviceClass} VID=0x${device.vendorId.toString(16)}")
                continue
            }

            Log.d(TAG, "Brute-force trying: id=${device.deviceId} VID=0x${device.vendorId.toString(16)} PID=0x${device.productId.toString(16)} class=${device.deviceClass}")

            for (dt in driverTypes) {
                if (count >= 2) break
                val conn = usbManager.openDevice(device) ?: break
                try {
                    val table = ProbeTable()
                    table.addProduct(device.vendorId, device.productId, dt)
                    val drivers = UsbSerialProber(table).findAllDrivers(usbManager)
                    if (drivers.isEmpty()) { conn.close(); continue }

                    val sp = drivers[0].ports[0]
                    sp.open(conn)
                    sp.setParameters(115200, 8, UsbSerialPort.STOPBITS_1, UsbSerialPort.PARITY_NONE)
                    assignPort(count, sp)
                    openedDeviceIds.add(device.deviceId)
                    count++
                    Log.d(TAG, "✓ Brute-force opened #$count with ${dt.simpleName}")
                    break
                } catch (e: Exception) {
                    Log.w(TAG, "Brute-force ${dt.simpleName} failed: ${e.message}")
                    conn.close()
                }
            }
        }

        Log.d(TAG, "Total: $count ESP32(s) connected")
        toast("$count ESP32(s) connected")
        updateStatus()
    }

    private fun assignPort(index: Int, sp: UsbSerialPort) {
        if (index == 0) {
            port1 = sp; device1Enabled = true
            findViewById<ToggleButton>(R.id.toggleDevice1).isEnabled = true
            findViewById<ToggleButton>(R.id.toggleDevice1).isChecked = true
        } else {
            port2 = sp; device2Enabled = true
            findViewById<ToggleButton>(R.id.toggleDevice2).isEnabled = true
            findViewById<ToggleButton>(R.id.toggleDevice2).isChecked = true
        }
    }

    private fun sendToDevices(num: String) {
        var sent = 0
        if (device1Enabled && port1 != null) {
            try { port1?.write("$num\n".toByteArray(), 200); sent++ }
            catch (_: Exception) {}
        }
        if (device2Enabled && port2 != null) {
            try { port2?.write("$num\n".toByteArray(), 200); sent++ }
            catch (_: Exception) {}
        }
        if (sent > 0) toast("Sent '$num' → $sent")
        else toast("No devices connected")
    }

    private fun updateStatus() {
        val d1 = if (port1 != null) "✓" else "✗"
        val d2 = if (port2 != null) "✓" else "✗"
        val t1 = if (device1Enabled) "ON" else "OFF"
        val t2 = if (device2Enabled) "ON" else "OFF"
        statusText.text = "ESP32 #1: $d1 ($t1) | ESP32 #2: $d2 ($t2)"
    }

    private fun toast(msg: String) = Toast.makeText(this, msg, Toast.LENGTH_SHORT).show()

    override fun onDestroy() {
        super.onDestroy()
        try { port1?.close(); port2?.close() } catch (_: Exception) {}
        port1 = null; port2 = null
        try { unregisterReceiver(usbPermissionReceiver) } catch (_: Exception) {}
    }
}
