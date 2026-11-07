# geotrio2mqtt
ESP firmware to read and publish meter readings from a Geo Trio II IHD.

The method below was originally developed on an ESP8266 NodeMCU, and also runs on ESP32 boards (tested on a Seeed Studio XIAO ESP32S3). The code is split into two files:
- `geotrio2mqtt.ino` - shared by all boards
- `platform.h` - the board-specific parts (libraries, meter RX pin, debug log port). The right section is picked automatically from the board you select in the Arduino IDE.

**HARDWARE REQUIREMENTS:**
- Geo Trio II in-home display (with or without stock WiFi module)
- ESP8266 or ESP32 board
- Soldering iron
- wire
- (Optional) USB-to-serial adaptor if you're using a bare ESP board or the existing Trio II WiFi module (ESP8285) 

**SOFTWARE REQUIREMENTS:**
- Arduino IDE
- Libraries (Tools > Manage Libraries):
  - ArduinoJson (>=7)
  - PubSubClient (>=2.8)
- The WiFi, web server, DNSServer, LittleFS and mDNS libraries come with the ESP8266 / ESP32 board package.
   
**COMPILING & FLASHING:**
- Install the required libraries in Arduino IDE (Tools > Manage Libraries)
- Add your board's repository:
  - Go to: File > Preferences > Additional Boards Manager URLs
  - ESP8266: paste http://arduino.esp8266.com/stable/package_esp8266com_index.json
  - ESP32: paste https://espressif.github.io/arduino-esp32/package_esp32_index.json
- Tools > Board > Board Manager > ESP8266 (or esp32) > Install
- Tools > Board > Then pick your particular board (e.g. NodeMCU 1.0, or XIAO_ESP32S3)
- ESP32-S3/C3 boards with native USB: Tools > USB CDC On Boot > Enabled (so debug logs appear over USB)
- Tools > Port > Select Serial port / USB (Make sure your board or USB serial adaptor is plugged in first)
- Sketch > Verify/Compile
- Put your ESP into flash mode (depends on board, could be automatic, could have a flash/reset button or ground GPIO0 etc)
- Sketch > Upload

**DISMANTLING IHD**

After removing the stand there are 2 screws to remove. One is visible, but the 2nd is well hidden. Firstly remove the white sticker (or paint?) then remove a small black plastic plug which I initially missed.

**WIRING:**
<img width="1002" height="715" alt="image" src="https://github.com/user-attachments/assets/82a61abf-7ac1-4d48-8289-fd4df3c74f73" />

- Trio II J8 (TX) <> ESP RX
  - ESP8266 NodeMCU: pin D7 (GPIO13)
  - Seeed XIAO ESP32S3: pin D8 (GPIO7). On ESP32 any free GPIO can be used - change `METER_RX_PIN` in `platform.h`
- GND <> GND
- 5V <> 5V
<img width="709" height="691" alt="image" src="https://github.com/user-attachments/assets/ebaee490-d495-40e9-97b9-8659a9a5374c" />

I routed the wires out the port on the back where the existing WiFi module usually sits.
In theory you could flash and reuse the ESP8285 found in the existing WiFi module and also reuse the existing pin header. This was my initial plan, but I realised the IHD will still transmit on the TX pin and that would clash with the TX pin that we jump from the zigbee chip. Therefore I found it easier to just use another ESP board (nodemcu) which is easy to flash and wire up.
  
**SETUP:**

If no WiFi has been configured the device will create a hotspot called geotrio2mqtt.
Once connected to it (no password), go to 192.168.4.1 to configure your WiFi SSID/password, MQTT broker, topic etc and then reboot.
The ESP should then connect to your WiFi and start publishing MQTT messages.
Holding the FLASH/BOOT button (GPIO0) at any time brings the setup hotspot back.

The Settings page also has:
- **Device name** - the name the ESP gives your router, and its web address: `http://geotrio2mqtt.local` by default. Handy for reserving a fixed IP address in your router.
- **Display** - the full scale of the electricity dial and gas flame on the web page (defaults: 15 kW and 25 kW, set by `ELEC_DIAL_MAX_KW` / `GAS_FLAME_MAX_KW` in the .ino).

Note: settings are stored in persistent memory meaning once set you can re-flash the ESP and it will reconnect to the same WiFi without requiring setup again.

**METER SCALING:**

Meters report readings in different units (e.g. one meter sends live power in watts, another in tenths of a watt). The meter sends its own multiplier/divisor when the Trio II starts up; geotrio2mqtt learns these, saves them and uses them from then on. Until then it uses the defaults in the .ino (`DEFAULT_ELEC_DIVISOR` / `DEFAULT_GAS_DIVISOR`).

If your readings are 10x (or 100x) out, power-cycle the Trio II once while the ESP is running. The scaling in use is shown in the Status line of the web page.

**MQTT Topics:**
- geotrio/elec/live_power_w
- geotrio/elec/meter (kWh)
- geotrio/gas/meter (m³)
- geotrio/elec/today_kwh, week_kwh, month_kwh
- geotrio/gas/today_kwh, week_kwh, month_kwh
- geotrio/elec/cost_today, cost_week, cost_month (£, including standing charge)
- geotrio/gas/cost_today, cost_week, cost_month (£, including standing charge)
- geotrio/elec/unit_rate, geotrio/gas/unit_rate (£/kWh)
- geotrio/elec/standing_charge, geotrio/gas/standing_charge (£/day)
- geotrio/gas/calorific_value (MJ/m³), geotrio/gas/conversion_factor (volume correction, e.g. 1.02264)
- geotrio/status (online/LWT offline)

Note: Elec topics are published every 10 seconds while gas every 2 minutes
This is the frequency transmitted by the smart meter itself so can't be changed AFAIK.

Also, despite gas readings being published every 2 minutes I believe they only update every 30 minutes.

The meter reports gas usage for today/week/month in kWh (converted by the meter system using the calorific value), while the gas meter reading itself is in m³.

The tariff (unit rates, standing charges, gas calorific value and volume correction) comes from price messages the hub sends about once an hour, so after first installing it can take up to an hour to appear. It's saved, so after that it shows straight away after a restart. These are your supplier's own figures: gas kWh = m³ × conversion_factor × calorific_value ÷ 3.6.

**WEBSERVER**
The ESP will also serve a small webpage with the live readings/usage. You can find this on port 80 at whatever IP address your router assigned it, or at `http://geotrio2mqtt.local`.
- Energy now: an electricity dial with the exact power below it, and a gas flame showing average gas power since the last gas update (the meter doesn't send live gas power, so this is calculated like the Trio II does)
- Electricity and gas meter readings
- Usage and cost for today, this week and this month
- Tariff: unit rates and standing charges, gas calorific value and volume correction
- Status: WiFi, MQTT, uptime, memory, meter data received and the scaling in use

The last gas values are saved and shown straight away after a restart, with how long ago they changed.

<img width="668" height="500" alt="image" src="https://github.com/user-attachments/assets/780e938f-5aae-4a52-9921-0dfccefa3991" />

You can also fetch a JSON object of the values from http://x.x.x.x/live
Example:
`{"elecW":168,"elecWage":"6s ago","elecK":14473.801,"elecKage":"6s ago","gasK":4236.845,"gasKage":"2 min ago","gasP":1.25,"gasPage":"8 min ago","h":[[5.2,40.1,120.4],[30.5,210.7,600.2]],"c":[[2.10,15.30,46.80],[2.25,13.70,39.10]],"t":[[24.500,48.000],[6.000,29.000]],"cv":39.80,"cf":1.02264,"scale":"elec /1000 (from meter), gas /1000 (from meter)","rssi":-71,"ssid":"MyFi","mqtt":1,"up":2618,"heap":43296,"rxB":81234,"rxF":2450,"rxD":240,"heapTotal":86096}`

`h` is usage in kWh and `c` is cost in £, each as [[elec today, week, month], [gas today, week, month]]. `t` is the tariff as [[elec unit rate p/kWh, elec standing charge p/day], [gas ...]].

**DEBUG LOG:**
- ESP32: the USB serial port (Serial Monitor at 115200)
- ESP8266: the meter uses the main serial port, so logs go to the TX-only UART1 on pin D4 (GPIO2) at 115200 - connect a USB-serial adaptor's RX to D4 to see them

Setting `DUMP_UNDECODED` to 1 in the .ino also logs every meter message that isn't decoded, which is useful for finding new data in the meter's messages.

**KNOWN ISSUES:**

I noticed some extremely large erroneous geotrio/gas/meter values.
These seemed to occur every few hours but the values before and after were correct.

I recommend setting a filter in HomeAssistant to ignore these values so they don't mess up your energy dashboard.
(I didn't see any erroneous values for the electricity topics but it may be worth applying the same fix to these too)

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
Alternatively, the geotrio/gas/today_kwh topic gives the meter's own kWh figure for today (it resets at midnight, which Home Assistant's `total_increasing` handles).
