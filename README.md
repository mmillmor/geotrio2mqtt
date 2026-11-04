# geotrio2mqtt
ESP firmware to read and publish meter readings from a Geo Trio II IHD.

The method below and the included .ino are based on using an ESP8266 NodeMCU because that's what I had on hand. It should work with any similar ESP8266 or ESP32 boards but may require some minor tweaks to the included libraries and the RX pin. Any AI should be able to point out any changes you need for your specific board.

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

**DISMANTLING IHD**

After removing the stand there are 2 screws to remove. One is visible, but the 2nd is well hidden. Firstly remove the white sticker (or paint?) then remove a small black plastic plug which I initially missed.

**WIRING:**
<img width="1002" height="715" alt="image" src="https://github.com/user-attachments/assets/82a61abf-7ac1-4d48-8289-fd4df3c74f73" />

- Trio II J8 (TX) <> ESP RX (Pin D7/GPIO13 for Nodemcu)
- GND <> GND
- 5V <> 5V
<img width="709" height="691" alt="image" src="https://github.com/user-attachments/assets/ebaee490-d495-40e9-97b9-8659a9a5374c" />

I routed the wires out the port on the back where the existing WiFi module usually sits.
In theory you could flash and reuse the ESP8285 found in the existing WiFi module and also reuse the existing pin header. This was my initial plan, but I realised the IHD will still transmit on the TX pin and that would clash with the TX pin that we jump from the zigbee chip. Therefore I found it easier to just use another ESP board (nodemcu) which is easy to flash and wire up.
  
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
If you need gas usage in kWh then you can use this template as an approximation (the actual calculation is more complex and varies by geographical location but is usually found on your bill)
```
  - name: "Gas Meter kWh"
    unique_id: gas_meter_kwh
    unit_of_measurement: "kWh"
    device_class: energy
    state_class: total_increasing
    state: "{{ (states('sensor.gas_meter') | float * 11.1868) | round(3) }}"
    availability: "{{ states('sensor.gas_meter') | is_number }}"
```  
