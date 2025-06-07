# Finding Your ESP32 Sensor in Any Network

This guide helps non-technical users easily find and access their ESP32 temperature sensor when plugged into foreign networks (hotels, offices, friends' homes, etc.).

## Quick Access Methods (Easiest to Hardest)

### Method 1: Use the Magic Address (.local) **RECOMMENDED**
- **What to do**: Open any web browser and type: `http://tr-cam1-t-h-sensor.local`
- **Works on**: Most modern networks (Windows 10+, Mac, Linux, phones)
- **Why it works**: Your device announces itself automatically
- **If it doesn't work**: Try Method 2

### Method 2: Check Router Admin Panel
- **What to do**: 
  1. Find the router's IP (usually printed on router sticker: 192.168.1.1 or 192.168.0.1)
  2. Open browser and go to that IP
  3. Look for "Connected Devices" or "DHCP Client List"
  4. Find device named "tr-cam1-t-h-sensor"
- **Works on**: All networks where you have router access
- **Why it works**: Your device tells the router its name

### Method 3: Network Scanner Apps
- **Android**: "Fing" or "Network Scanner" (free apps)
- **iPhone**: "Fing" or "Network Analyzer" 
- **Computer**: "Advanced IP Scanner" (Windows) or "LanScan" (Mac)
- **What to look for**: Device named "tr-cam1-t-h-sensor" or ESP32-related

### Method 4: Check Serial Monitor (Technical)
- **What to do**: Connect USB cable and check Arduino IDE serial monitor
- **Baud rate**: 115200
- **Look for**: Big box with IP address after connection
- **For**: Technical users or when other methods fail

## LED Status Indicators

Your ESP32 has a built-in LED that shows connection status:

| LED Pattern | Meaning |
|-------------|---------|
| 2 slow blinks | Trying to connect to network |
| 3 quick blinks | Successfully connected |
| 1 long blink | Network disconnected |
| 1 quick blink every 5 min | Taking sensor reading |

## Troubleshooting Guide

### "tr-cam1-t-h-sensor.local" doesn't work?
1. **Try with different browsers**: Chrome, Firefox, Safari, Edge
2. **Check if on same network**: Make sure your phone/computer is on same WiFi
3. **Wait 2-3 minutes**: Device needs time to announce itself
4. **Try with .local at end**: Make sure you typed `.local` not `.com`

### Can't find in router admin panel?
1. **Look for different names**: "ESP32", "Espressif", or the device MAC address
2. **Check all tabs**: Look in "Wireless", "LAN", "DHCP" sections
3. **Wait and refresh**: Device might take time to appear

### Network scanner shows nothing?
1. **Make sure on same network**: Scanner and ESP32 must be on same WiFi/LAN
2. **Try different apps**: Some work better than others
3. **Check if network allows discovery**: Some corporate networks block device discovery

### Still can't find it?
1. **Check LED status**: Is it showing "connected" pattern (3 quick blinks)?
2. **Try USB cable**: Connect to computer and check serial output
3. **Power cycle**: Unplug and replug the ESP32
4. **Check WiFi name**: Make sure it's connecting to the right network (look at source code)

## Mobile Access Tips

- **Bookmark it**: Once you find the address, bookmark `http://tr-cam1-t-h-sensor.local`
- **Add to home screen**: Most browsers let you add web pages as app icons
- **Works offline**: Once the page loads, charts work without internet

## Network Compatibility

| Network Type | .local Access | Router Access | Scanner Apps |
|--------------|---------------|---------------|--------------|
| Home WiFi | Yes | Yes | Yes |
| Hotel WiFi | Maybe | No | Maybe |
| Office WiFi | Maybe | Maybe | Maybe |
| Mobile Hotspot | Yes | Yes | Yes |
| Public WiFi | Usually No | No | Usually No |

## Security Note

Your device only works on the local network - it's not accessible from the internet, making it safe to use on foreign networks.

## Pro Tips

1. **Take a photo** of the serial output showing the IP address when first setting up
2. **Write down the WiFi name** your device connects to (from source code)
3. **Test at home first** to make sure everything works
4. **Use mobile hotspot** as backup if other networks don't work
5. **The LED is your friend** - watch the blink patterns to know what's happening

---

**Need help?** If none of these methods work, the device might not be connected to the network. Check the power supply and make sure the WiFi credentials in the code match the network you're trying to use. 