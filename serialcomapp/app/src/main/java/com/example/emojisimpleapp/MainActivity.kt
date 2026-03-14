package com.example.emojisimpleapp

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbManager
import android.os.Build
import android.os.Bundle
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
                            toast("Permission granted!")
                            statusText.postDelayed({ requestPermissionsAndConnect() }, 300)
                        } else {
                            toast("Permission denied. Tap Reconnect.")
                            updateStatus()
                        }
                    }
                }
            } catch (e: Exception) {
                toast("Permission error: ${e.message}")
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

    private fun findAllSerialDevices(): List<UsbSerialDriver> {
        val result = mutableListOf<UsbSerialDriver>()
        val matchedIds = mutableSetOf<Int>()

        // Default prober catches well-known VID/PIDs
        for (d in UsbSerialProber.getDefaultProber().findAllDrivers(usbManager)) {
            result.add(d); matchedIds.add(d.device.deviceId)
        }

        // Brute-force remaining USB devices with all driver types
        // Skip hubs (class 9), HID (class 3), mass storage (class 8)
        val skipClasses = setOf(0, 3, 8, 9)
        val driverTypes = listOf(
            CdcAcmSerialDriver::class.java,
            Ch34xSerialDriver::class.java,
            Cp21xxSerialDriver::class.java,
            FtdiSerialDriver::class.java
        )
        for (device in usbManager.deviceList.values) {
            if (device.deviceId in matchedIds) continue
            if (device.deviceClass in skipClasses) continue
            for (dt in driverTypes) {
                try {
                    val table = ProbeTable()
                    table.addProduct(device.vendorId, device.productId, dt)
                    val found = UsbSerialProber(table).findAllDrivers(usbManager)
                    if (found.isNotEmpty()) {
                        result.addAll(found); matchedIds.add(device.deviceId); break
                    }
                } catch (_: Exception) {}
            }
        }
        return result
    }

    private fun requestPermissionsAndConnect() {
        if (permissionRequested) return
        val serialDevices = findAllSerialDevices()
        toast("Found ${serialDevices.size} serial device(s)")
        if (serialDevices.isEmpty()) {
            toast("No ESP32 found. Check USB hub.")
            updateStatus(); return
        }
        // Request permission for serial devices only
        for (driver in serialDevices) {
            if (!usbManager.hasPermission(driver.device)) {
                val name = driver.device.productName ?: "VID=${driver.device.vendorId}"
                permissionRequested = true
                toast("Requesting permission: $name")
                val intent = Intent(ACTION_USB_PERMISSION).apply { setPackage(packageName) }
                val flags = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S)
                    PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
                else PendingIntent.FLAG_UPDATE_CURRENT
                usbManager.requestPermission(driver.device, PendingIntent.getBroadcast(this, 0, intent, flags))
                return
            }
        }
        // All have permission
        openSerialPorts(serialDevices)
    }

    private fun openSerialPorts(serialDevices: List<UsbSerialDriver>) {
        var count = 0
        for (driver in serialDevices) {
            if (count >= 2) break
            val conn = usbManager.openDevice(driver.device)
            if (conn == null) { toast("Can't open device"); continue }
            for (sp in driver.ports) {
                if (count >= 2) break
                try {
                    sp.open(conn)
                    sp.setParameters(115200, 8, UsbSerialPort.STOPBITS_1, UsbSerialPort.PARITY_NONE)
                    if (count == 0) {
                        port1 = sp; device1Enabled = true
                        findViewById<ToggleButton>(R.id.toggleDevice1).isEnabled = true
                        findViewById<ToggleButton>(R.id.toggleDevice1).isChecked = true
                    } else {
                        port2 = sp; device2Enabled = true
                        findViewById<ToggleButton>(R.id.toggleDevice2).isEnabled = true
                        findViewById<ToggleButton>(R.id.toggleDevice2).isChecked = true
                    }
                    count++; toast("ESP32 #$count ✓")
                } catch (e: Exception) {
                    toast("Open failed: ${e.message}")
                }
            }
        }
        toast("$count device(s) connected")
        updateStatus()
    }

    private fun sendToDevices(num: String) {
        var sent = 0
        if (device1Enabled && port1 != null) {
            try { port1?.write("$num\n".toByteArray(), 200); sent++ }
            catch (e: Exception) { toast("D1 fail: ${e.message}") }
        }
        if (device2Enabled && port2 != null) {
            try { port2?.write("$num\n".toByteArray(), 200); sent++ }
            catch (e: Exception) { toast("D2 fail: ${e.message}") }
        }
        if (sent > 0) toast("Sent '$num' → $sent device(s)")
        else toast("No devices connected")
    }

    private fun updateStatus() {
        val d1Connected = if (port1 != null) "✓" else "✗"
        val d2Connected = if (port2 != null) "✓" else "✗"
        val d1Target = if (device1Enabled) "ON" else "OFF"
        val d2Target = if (device2Enabled) "ON" else "OFF"
        statusText.text = "ESP32 #1: $d1Connected ($d1Target) | ESP32 #2: $d2Connected ($d2Target)"
    }

    private fun toast(msg: String) = Toast.makeText(this, msg, Toast.LENGTH_SHORT).show()

    override fun onDestroy() {
        super.onDestroy()
        try {
            port1?.close()
            port2?.close()
            port1 = null
            port2 = null
        } catch (e: Exception) {
            // Ignore close errors
        }
        try {
            unregisterReceiver(usbPermissionReceiver)
        } catch (e: Exception) {
            // Receiver might not be registered
        }
    }
}
