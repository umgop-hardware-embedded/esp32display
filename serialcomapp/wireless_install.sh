#!/bin/bash

# Wireless ADB install script
# Usage: ./wireless_install.sh <TABLET_IP_ADDRESS>

if [ $# -eq 0 ]; then
    echo "Usage: ./wireless_install.sh <TABLET_IP_ADDRESS>"
    echo ""
    echo "To find your tablet's IP address:"
    echo "1. Go to Settings on the tablet"
    echo "2. Look for Wi-Fi settings or About Device"
    echo "3. Find the IP address in the hotspot connection details"
    echo ""
    exit 1
fi

TABLET_IP=$1
ADB="/Users/umeshgopi/Library/Android/sdk/platform-tools/adb"
APK="app/build/outputs/apk/debug/app-debug.apk"
PORT=5555

echo "🔧 Setting up wireless ADB connection to $TABLET_IP:$PORT"
echo ""

# Connect to the tablet
echo "1️⃣  Connecting to tablet at $TABLET_IP..."
$ADB connect $TABLET_IP:$PORT

# Give it a moment to establish connection
sleep 2

# Check if connected
echo ""
echo "2️⃣  Verifying connection..."
$ADB devices

# Install the APK
echo ""
echo "3️⃣  Installing app..."
$ADB -s $TABLET_IP:$PORT install -r "$APK"

if [ $? -eq 0 ]; then
    echo ""
    echo "✅ Installation successful!"
    echo "🚀 Launching app..."
    $ADB -s $TABLET_IP:$PORT shell am start -n com.example.emojisimpleapp/.MainActivity
else
    echo ""
    echo "❌ Installation failed. Check the IP address and ensure USB Debugging is enabled."
fi
