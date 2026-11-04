# geotrio2mqtt
ESP firmware to read and publish meter readings from a Geo Trio II IHD

**HARDWARE REQUIREMENTS:**
- Geo Trio II in-home display (with or without stock WiFi module)
- ESP8266 (or ESP32 etc)
- Soldering iron
- wire
- (Optional) USB-to-serial adaptor if you're using a bare ESP board or the existing Trio II WiFi module (ESP8285) 

**SOFTWARE REQUIREMENTS:**
- Arduino IDE
- Libraries:
  - ESP8266WiFi
  - ESP8266WebServer
  - DNSServer
  - LittleFS
  - ArduinoJson (>=7)
  - PubSubClient (>=2.8)
   
**COMPILING & FLASHING:**
- Install required packages in Arduino IDE (Tools > Manage Libraries)
- Add ESP8266 boards repository:
  - Go to: File > Preferences > Additional Boards Manager URLs
  - Paste http://arduino.esp8266.com/stable/package_esp8266com_index.json
- Tools > Board > Board Manager > ESP8266 > Install
- Tools > Board > ESP8266 Boards > Then pick your particular variant
- Tools > Port > Select Serial port / USB (Make sure your USB serial adaptor is plugged in first)
- Configure WiFi SSID & password, broker IP etc in .ino file
- Sketch > Verify/Compile
- Put your ESP into flash mode (depends on board, could be automatic, could have a flash/reset button or ground GPIO0 etc)
- Sketch > Upload 

**WIRING (See images for locations):**
- Trio II J8 (TX) <> ESP RX (Pin D7/GPIO13 for Nodemcu)
- GND <> GND
- 5V <> 5V (or 3V3 <> 3V3 depending on your board)
 
**SETUP:**

If no WiFi was configured in this script then the device will create a hotspot called geotrio2mqtt.
Once connected to it (no password), go to 192.168.4.1 to configure your WiFi SSID/password, MQTT topic etc and then reboot.
The ESP should then connect to your WiFi and start publishing MQTT messages.

Note: the WiFi SSID, password and topic etc are stored in persistent memory meaning once set you can re-flash the ESP and it will reconnect to the same WiFi without requiring setup again.

**MQTT Topics:**
- geotrio/elec/live_power_w
- geotrio/elec/meter
- geotrio/gas/meter
- geotrio/status (online/LWT offline)
Note: Elec topics are published every 10 seconds while gas every 2 minutes
This is the frequency transmitted by the smart meter itself so can't be changed AFAIK.

Also, despite gas readings being published every 2 minutes I believe they only update every 30 minutes.

**KNOWN ISSUES:**

I noticed some extremely large erroneous geotrio/gas/meter values.
These seemed to occur every few hours but the previous and next values were correct.

I recommend setting a filter in HomeAssistant to ignore these values.
(I didn't see any erroneous values for the electricity topics but it may be worth apply the same fix to these too)

e.g in mqtt.yaml
```
- sensor:
  - name: "Gas Meter"
    unique_id: gas_meter
    state_topic: "geotrio/gas/meter"
    unit_of_measurement: "m³"
    device_class: gas
    state_class: total_increasing
    value_template: >-
      {% set reading = value | float(none) %}
      {% if reading is not none and 0 <= reading <= 1000000 %}
        {{ reading }}
      {% else %}
        {{ this.state }}
      {% endif %}
```
