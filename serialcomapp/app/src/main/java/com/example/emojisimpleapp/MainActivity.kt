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

    private fun requestUsbPermission() {
        try {
            if (permissionRequested) {
                toast("Permission already requested, please wait...")
                return
            }
            
            val drivers = UsbSerialProber.getDefaultProber().findAllDrivers(usbManager)
            if (drivers.isEmpty()) {
                // Also check raw USB device list for unprobed devices
                val deviceList = usbManager.deviceList
                toast("No serial drivers. Raw USB devices: ${deviceList.size}")
                updateStatus()
                permissionRequested = false
                return
            }
            
            toast("Found ${drivers.size} serial driver(s)")
            
            // Collect unique USB devices that need permission
            val devicesNeedingPermission = drivers
                .map { it.device }
                .distinctBy { it.deviceId }
                .filter { !usbManager.hasPermission(it) }
            
            if (devicesNeedingPermission.isNotEmpty()) {
                val device = devicesNeedingPermission[0]
                val deviceName = device.productName ?: device.deviceName ?: "Unknown"
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
        val drivers = UsbSerialProber.getDefaultProber().findAllDrivers(usbManager)
        if (drivers.isEmpty()) {
            toast("No USB serial devices found")
            updateStatus()
            return
        }
        
        toast("Found ${drivers.size} driver(s)")
        
        // Collect all available ports from all drivers
        val allPorts = mutableListOf<Pair<UsbSerialPort, android.hardware.usb.UsbDevice>>()
        for (driver in drivers) {
            for (port in driver.ports) {
                allPorts.add(Pair(port, driver.device))
            }
        }
        
        toast("Found ${allPorts.size} total port(s)")
        
        var connectedCount = 0
        
        // Open up to 2 ports
        for (i in 0 until minOf(2, allPorts.size)) {
            try {
                val (port, device) = allPorts[i]
                
                val connection = usbManager.openDevice(device)
                if (connection == null) {
                    toast("Failed to open device ${i+1} - no permission?")
                    continue
                }
                
                port.open(connection)
                port.setParameters(115200, 8, UsbSerialPort.STOPBITS_1, UsbSerialPort.PARITY_NONE)
                
                if (connectedCount == 0) {
                    port1 = port
                    device1Enabled = true
                    findViewById<ToggleButton>(R.id.toggleDevice1).isEnabled = true
                    findViewById<ToggleButton>(R.id.toggleDevice1).isChecked = true
                } else if (connectedCount == 1) {
                    port2 = port
                    device2Enabled = true
                    findViewById<ToggleButton>(R.id.toggleDevice2).isEnabled = true
                    findViewById<ToggleButton>(R.id.toggleDevice2).isChecked = true
                }
                
                connectedCount++
                toast("Connected ESP32 #$connectedCount")
            } catch (e: Exception) {
                toast("Error device ${i+1}: ${e.message}")
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
