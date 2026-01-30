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
import androidx.appcompat.app.AppCompatActivity
import com.hoho.android.usbserial.driver.UsbSerialPort
import com.hoho.android.usbserial.driver.UsbSerialProber

class MainActivity : AppCompatActivity() {

    private val ACTION_USB_PERMISSION = "com.example.emojisimpleapp.USB_PERMISSION"
    private lateinit var usbManager: UsbManager
    private var port: UsbSerialPort? = null
    private lateinit var statusText: TextView
    private var permissionRequested = false

    private val usbPermissionReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            try {
                if (intent.action == ACTION_USB_PERMISSION) {
                    synchronized(this) {
                        val granted = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)
                        if (granted) {
                            updateStatus("Permission granted, connecting...")
                            toast("Permission granted! Connecting...")
                            // Small delay to ensure permission is fully processed
                            statusText.postDelayed({ 
                                openSerial()
                            }, 300)
                        } else {
                            updateStatus("Permission denied")
                            toast("USB permission denied. Tap 'Reconnect ESP32' to try again.")
                            permissionRequested = false
                        }
                    }
                }
            } catch (e: Exception) {
                toast("Permission error: ${e.message}")
                updateStatus("Permission error")
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
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                registerReceiver(usbPermissionReceiver, intentFilter, Context.RECEIVER_NOT_EXPORTED)
            } else {
                registerReceiver(usbPermissionReceiver, intentFilter)
            }

            findViewById<Button>(R.id.btnReconnect).setOnClickListener { 
                try {
                    port?.close()
                    port = null
                    permissionRequested = false
                    updateStatus("Reconnecting...")
                    toast("Attempting to reconnect...")
                    statusText.postDelayed({ requestUsbPermission() }, 500)
                } catch (e: Exception) {
                    toast("Reconnect failed: ${e.message}")
                    permissionRequested = false
                }
            }
            findViewById<Button>(R.id.btn1).setOnClickListener { send("1") }
            findViewById<Button>(R.id.btn2).setOnClickListener { send("2") }
            findViewById<Button>(R.id.btn3).setOnClickListener { send("3") }
            findViewById<Button>(R.id.btn4).setOnClickListener { send("4") }

            // Longer delay to ensure system is stable
            updateStatus("Initializing...")
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
                updateStatus("No USB device found")
                toast("No ESP32 found. Please connect via USB OTG.")
                permissionRequested = false
                return
            }
            
            val device = drivers[0].device
            val deviceName = device.productName ?: device.deviceName ?: "Unknown"
            toast("Found: $deviceName")
            
            if (usbManager.hasPermission(device)) {
                updateStatus("Already have permission")
                toast("Already have permission, connecting...")
                permissionRequested = false
                openSerial()
            } else {
                permissionRequested = true
                updateStatus("Waiting for permission...")
                toast("Grant permission in the dialog")
                val intent = Intent(ACTION_USB_PERMISSION)
                intent.setPackage(packageName)
                val flags = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                    PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
                } else {
                    PendingIntent.FLAG_UPDATE_CURRENT
                }
                val pi = PendingIntent.getBroadcast(this, 0, intent, flags)
                usbManager.requestPermission(device, pi)
            }
        } catch (e: Exception) {
            updateStatus("Error: ${e.message}")
            toast("Error requesting permission: ${e.message}")
            permissionRequested = false
        }
    }

    private fun openSerial() {
        val drivers = UsbSerialProber.getDefaultProber().findAllDrivers(usbManager)
        if (drivers.isEmpty()) {
            updateStatus("No drivers found")
            toast("No USB drivers found")
            return
        }
        
        val driver = drivers[0]
        val device = driver.device
        
        val connection = usbManager.openDevice(device)
        if (connection == null) {
            updateStatus("Failed to open device")
            toast("Failed to open USB device. Try unplugging and reconnecting.")
            return
        }
        
        try {
            port = driver.ports[0]
            port?.open(connection)
            port?.setParameters(115200, 8, UsbSerialPort.STOPBITS_1, UsbSerialPort.PARITY_NONE)
            updateStatus("Connected ✓")
            toast("Connected to ESP32 successfully!")
        } catch (e: Exception) {
            port = null
            connection.close()
            updateStatus("Connection failed")
            toast("Connection error: ${e.message}")
        }
    }

    private fun send(num: String) {
        if (port != null) {
            try {
                val data = "$num\n"
                port?.write(data.toByteArray(), 200)
                toast("Sent: $num")
            } catch (e: Exception) {
                toast("Send failed: ${e.message}")
            }
        } else {
            toast("Not connected - Please connect ESP32")
        }
    }

    private fun updateStatus(status: String) {
        statusText.text = "Status: $status"
    }

    private fun toast(msg: String) = Toast.makeText(this, msg, Toast.LENGTH_SHORT).show()

    override fun onDestroy() {
        super.onDestroy()
        try {
            port?.close()
            port = null
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
