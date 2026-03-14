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
import com.hoho.android.usbserial.driver.UsbSerialPort
import com.hoho.android.usbserial.driver.UsbSerialProber
import com.hoho.android.usbserial.driver.CdcAcmSerialDriver
import com.hoho.android.usbserial.driver.Ch34xSerialDriver
import com.hoho.android.usbserial.driver.Cp21xxSerialDriver
import com.hoho.android.usbserial.driver.FtdiSerialDriver
import com.hoho.android.usbserial.driver.ProbeTable

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
                        val granted = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)
                        if (granted) {
                            updateStatus()
                            toast("Permission granted! Connecting...")
                            permissionRequested = false
                            // Re-request to handle second device, or open ports if all done
                            statusText.postDelayed({ 
                                requestUsbPermission()
                            }, 300)
                        } else {
                            updateStatus()
                            toast("USB permission denied. Tap 'Reconnect' to try again.")
                            permissionRequested = false
                        }
                    }
                }
            } catch (e: Exception) {
                toast("Permission error: ${e.message}")
                updateStatus()
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
            
            val intentFilter = IntentFilter(ACTION_USB_PERMISSION)
            ContextCompat.registerReceiver(
                this,
                usbPermissionReceiver,
                intentFilter,
                ContextCompat.RECEIVER_NOT_EXPORTED
            )

            // Toggle buttons for device control
            findViewById<ToggleButton>(R.id.toggleDevice1).setOnCheckedChangeListener { _, isChecked ->
                device1Enabled = isChecked
                updateStatus()
            }
            findViewById<ToggleButton>(R.id.toggleDevice2).setOnCheckedChangeListener { _, isChecked ->
                device2Enabled = isChecked
                updateStatus()
            }

            findViewById<Button>(R.id.btnReconnect).setOnClickListener { 
                try {
                    port1?.close()
                    port2?.close()
                    port1 = null
                    port2 = null
                    permissionRequested = false
                    updateStatus()
                    toast("Attempting to reconnect...")
                    statusText.postDelayed({ requestUsbPermission() }, 500)
                } catch (e: Exception) {
                    toast("Reconnect failed: ${e.message}")
                    permissionRequested = false
                }
            }
            findViewById<Button>(R.id.btn1).setOnClickListener { sendToDevices("1") }
            findViewById<Button>(R.id.btn2).setOnClickListener { sendToDevices("2") }
            findViewById<Button>(R.id.btn3).setOnClickListener { sendToDevices("3") }
            findViewById<Button>(R.id.btn4).setOnClickListener { sendToDevices("4") }

            // Longer delay to ensure system is stable
            statusText.postDelayed({ 
                if (!isFinishing) {
                    requestUsbPermission()
                }
            }, 1000)
        } catch (e: Exception) {
            Toast.makeText(this, "Error initializing app: ${e.message}", Toast.LENGTH_LONG).show()
            finish()
        }
    }

    private fun getAllSerialDrivers(): List<com.hoho.android.usbserial.driver.UsbSerialDriver> {
        // First try default prober
        val defaultDrivers = UsbSerialProber.getDefaultProber().findAllDrivers(usbManager)
        val matchedDeviceIds = defaultDrivers.map { it.device.deviceId }.toSet()
        
        // Then try every unmatched USB device with all known driver types
        val customTable = ProbeTable()
        val allDevices = usbManager.deviceList.values
        
        toast("Hub: ${allDevices.size} USB device(s) total")
        
        val extraDrivers = mutableListOf<com.hoho.android.usbserial.driver.UsbSerialDriver>()
        for (device in allDevices) {
            if (device.deviceId in matchedDeviceIds) continue
            
            // Try each driver type on unmatched devices
            val driverTypes = listOf(
                CdcAcmSerialDriver::class.java,
                Cp21xxSerialDriver::class.java,
                Ch34xSerialDriver::class.java,
                FtdiSerialDriver::class.java
            )
            
            for (driverType in driverTypes) {
                try {
                    val table = ProbeTable()
                    table.addProduct(device.vendorId, device.productId, driverType)
                    val prober = UsbSerialProber(table)
                    val found = prober.findAllDrivers(usbManager)
                    if (found.isNotEmpty()) {
                        extraDrivers.addAll(found)
                        toast("Extra device found: VID=${device.vendorId} PID=${device.productId}")
                        break
                    }
                } catch (_: Exception) {}
            }
        }
        
        return defaultDrivers + extraDrivers
    }

    private fun requestUsbPermission() {
        try {
            if (permissionRequested) {
                toast("Permission already requested, please wait...")
                return
            }
            
            // Request permission for ALL USB devices on the hub
            val allDevices = usbManager.deviceList.values
            if (allDevices.isEmpty()) {
                toast("No USB devices found on hub")
                updateStatus()
                return
            }
            
            toast("${allDevices.size} USB device(s) on hub")
            
            val needsPermission = allDevices.filter { !usbManager.hasPermission(it) }
            
            if (needsPermission.isNotEmpty()) {
                val device = needsPermission.first()
                val deviceName = device.productName ?: "VID=${device.vendorId}"
                permissionRequested = true
                toast("Requesting permission for: $deviceName")
                val intent = Intent(ACTION_USB_PERMISSION)
                intent.setPackage(packageName)
                val flags = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                    PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
                } else {
                    PendingIntent.FLAG_UPDATE_CURRENT
                }
                val pi = PendingIntent.getBroadcast(this, 0, intent, flags)
                usbManager.requestPermission(device, pi)
                return
            }
            
            // All devices have permission — open them
            openSerialPorts()
        } catch (e: Exception) {
            updateStatus()
            toast("Error: ${e.message}")
            permissionRequested = false
        }
    }

    private fun openSerialPorts() {
        // First: open devices matched by default prober
        val defaultDrivers = UsbSerialProber.getDefaultProber().findAllDrivers(usbManager)
        val matchedDeviceIds = mutableSetOf<Int>()
        
        var connectedCount = 0
        
        // Open default-probed devices
        for (driver in defaultDrivers) {
            if (connectedCount >= 2) break
            matchedDeviceIds.add(driver.device.deviceId)
            try {
                val connection = usbManager.openDevice(driver.device) ?: continue
                val port = driver.ports[0]
                port.open(connection)
                port.setParameters(115200, 8, UsbSerialPort.STOPBITS_1, UsbSerialPort.PARITY_NONE)
                
                if (connectedCount == 0) {
                    port1 = port
                    device1Enabled = true
                    findViewById<ToggleButton>(R.id.toggleDevice1).isEnabled = true
                    findViewById<ToggleButton>(R.id.toggleDevice1).isChecked = true
                } else {
                    port2 = port
                    device2Enabled = true
                    findViewById<ToggleButton>(R.id.toggleDevice2).isEnabled = true
                    findViewById<ToggleButton>(R.id.toggleDevice2).isChecked = true
                }
                connectedCount++
                toast("ESP32 #$connectedCount connected (auto)")
            } catch (e: Exception) {
                toast("Default driver failed: ${e.message}")
            }
        }
        
        // Then: for remaining USB devices, brute-force try every driver at open time
        if (connectedCount < 2) {
            val driverClasses = listOf(
                CdcAcmSerialDriver::class.java,
                Cp21xxSerialDriver::class.java,
                Ch34xSerialDriver::class.java,
                FtdiSerialDriver::class.java
            )
            
            for (device in usbManager.deviceList.values) {
                if (connectedCount >= 2) break
                if (device.deviceId in matchedDeviceIds) continue
                
                val connection = usbManager.openDevice(device)
                if (connection == null) {
                    toast("No permission for VID=${device.vendorId}")
                    continue
                }
                
                var opened = false
                for (driverClass in driverClasses) {
                    try {
                        val table = ProbeTable()
                        table.addProduct(device.vendorId, device.productId, driverClass)
                        val prober = UsbSerialProber(table)
                        val drivers = prober.findAllDrivers(usbManager)
                        if (drivers.isEmpty()) continue
                        
                        val port = drivers[0].ports[0]
                        port.open(connection)
                        port.setParameters(115200, 8, UsbSerialPort.STOPBITS_1, UsbSerialPort.PARITY_NONE)
                        
                        if (connectedCount == 0) {
                            port1 = port
                            device1Enabled = true
                            findViewById<ToggleButton>(R.id.toggleDevice1).isEnabled = true
                            findViewById<ToggleButton>(R.id.toggleDevice1).isChecked = true
                        } else {
                            port2 = port
                            device2Enabled = true
                            findViewById<ToggleButton>(R.id.toggleDevice2).isEnabled = true
                            findViewById<ToggleButton>(R.id.toggleDevice2).isChecked = true
                        }
                        connectedCount++
                        toast("ESP32 #$connectedCount via ${driverClass.simpleName}")
                        opened = true
                        break
                    } catch (_: Exception) {
                        // This driver type didn't work, try next
                    }
                }
                
                if (!opened) {
                    connection.close()
                    toast("No driver worked for VID=${device.vendorId} PID=${device.productId}")
                }
            }
        }
        
        toast("$connectedCount device(s) connected")
        updateStatus()
    }

    private fun sendToDevices(num: String) {
        var sentCount = 0
        var failedCount = 0

        // Send to Device 1 if enabled
        if (device1Enabled && port1 != null) {
            try {
                val data = "$num\n"
                port1?.write(data.toByteArray(), 200)
                sentCount++
            } catch (e: Exception) {
                toast("Device 1 send failed: ${e.message}")
                failedCount++
            }
        }

        // Send to Device 2 if enabled
        if (device2Enabled && port2 != null) {
            try {
                val data = "$num\n"
                port2?.write(data.toByteArray(), 200)
                sentCount++
            } catch (e: Exception) {
                toast("Device 2 send failed: ${e.message}")
                failedCount++
            }
        }

        when {
            sentCount == 0 && failedCount == 0 -> 
                toast("No devices connected - Please connect ESP32s")
            sentCount > 0 -> 
                toast("Sent '$num' to $sentCount device(s)")
            failedCount > 0 -> 
                toast("Failed to send to some devices")
        }
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
