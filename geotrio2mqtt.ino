/*
 * geotrio2mqtt.ino - https://github.com/nadnerb33/geotrio2mqtt/
 * -------------------------------------------------------------------------
 * Geo Trio II smart meter display -> MQTT
 *
 * HARDWARE REQUIREMENTS:
 * - Geo Trio II in-home display (with or without stock wifi module)
 * - ESP8266 (or ESP32 etc)
 * - Soldering iron
 * - wire
 * - (Optional) USB-to-serial adaptor if you're using a bare ESP board or the existing Trio II wifi module (ESP8285)
 * 
 * SOFTWARE REQUIREMENTS:
 *  - Arduino IDE
 *  - ESP8266WiFi
 *  - ESP8266WebServer
 *  - DNSServer
 *  - LittleFS
 *  - ArduinoJson (>=7)
 *  - PubSubClient (>=2.8)
 *  
 * COMPILING & FLASHING:
 * - Install required packages in Arduino IDE (Tools > Manage Libraries)
 * - Add ESP8266 boards repository:
 *     - Go to: File > Preferences > Additional Boards Manager URLs
 *     - Paste http://arduino.esp8266.com/stable/package_esp8266com_index.json
 * - Tools > Board > Board Manager > ESP8266 > Install
 * - Tools > Board > ESP8266 Boards > Then pick your particular variant
 * - Tools > Port > Select Serial port / USB (Make sure your USB serial adaptor is plugged in first)
 * - Configure WiFi SSID & password in .ino file
 * - Sketch > Verify/Compile
 * - Put your ESP into flash mode (depends on board, could be automatic, could have a flash/reset button or ground GPIO0 etc)
 * - Sketch > Upload 
 *
 * WIRING (See images for locations):
 * - Trio II J8 (TX) <> ESP RX (Pin D7/GPIO13 for Nodemcu)
 * - GND <> GND
 * - 5V <> 5V (or 3V3 <> 3V3 depending on your board)
 * 
 * SETUP:
 * If no WiFi was configured in this script then the device will create a hotspot called geotrio2mqtt.
 * Once connected to it (no password), go to 192.168.4.1 to configure your WiFi SSID/password, MQTT topic etc and then reboot.
 * The ESP should then connect to your WiFi and start publishing MQTT messages.
 * Note: the WiFi SSID, password and topic etc are stored in perisitent memory 
 * meaning once set you can reflash the ESP and it will reconnect to the same WiFi without requiring setup again.
 *
 * MQTT Topics:
 * - <base>/elec/live_power_w
 * - <base>/elec/meter
 * - <base>/gas/meter
 * - <base>/status (online/LWT offline)
 * Note: Elec topics are published every 10 seconds while gas every 2 minutes
 * This is the frequency transmitted by the smart meter itself so can't be changed AFAIK.
 * Also, despite gas readings being published every 2 minutes I believe they only update every 30 minutes.
 *
 * KNOWN ISSUES:
 * I noticed some extremely large erroneous geotrio/gas/meter values. These seemed to occur every few hours but the previous and next values were correct.
 * I recommend setting a filter in HomeAssistant to ignore these values. (I didn't see any erroneous values for the electricity topics but it may be worth apply the same fix to these too)
 * e.g in mqtt.yaml
 *   - sensor:
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
 * -------------------------------------------------------------------------
 */
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>

struct Config {
  // Add your wifi details here
  char ssid[33] = "", wpass[65] = "";
  
  // MQTT broker details
  char host[40] = "192.168.0.33", port[6] = "1883", user[32] = "", mpass[32] = "";
  
  // MQTT topic base e.g <geotrio>/elec/live_power_w
  char base[48] = "geotrio";
  
  char baud[8] = "115200";
} cfg;
const char *CFG="/config.json";

ESP8266WebServer web(80);
DNSServer dns;
WiFiClient net;
PubSubClient mqtt(net);
bool apMode=false;
char statusTopic[64], nodeId[20];
volatile double vElecW=NAN, vElecK=NAN, vGasK=NAN;
unsigned long tElecW=0, tElecK=0, tGasK=0, bootMs=0, lastMqtt=0;

// ---------- config persistence ----------
void loadCfg() {
  if(!LittleFS.begin()) {
    LittleFS.format();
    LittleFS.begin();
  }
  File f=LittleFS.open(CFG,"r");
  if(!f) return;
  JsonDocument d;
  if(!deserializeJson(d,f)) {
    strlcpy(cfg.ssid,d["ssid"]|cfg.ssid,sizeof cfg.ssid);
    strlcpy(cfg.wpass,d["wpass"]|cfg.wpass,sizeof cfg.wpass);
    strlcpy(cfg.host,d["host"]|cfg.host,sizeof cfg.host);
    strlcpy(cfg.port,d["port"]|cfg.port,sizeof cfg.port);
    strlcpy(cfg.user,d["user"]|cfg.user,sizeof cfg.user);
    strlcpy(cfg.mpass,d["mpass"]|cfg.mpass,sizeof cfg.mpass);
    strlcpy(cfg.base,d["base"]|cfg.base,sizeof cfg.base);
    strlcpy(cfg.baud,d["baud"]|cfg.baud,sizeof cfg.baud);
  }
  f.close();
}

void saveCfg() {
  JsonDocument d;
  d["ssid"]=cfg.ssid;
  d["wpass"]=cfg.wpass;
  d["host"]=cfg.host;
  d["port"]=cfg.port;
  d["user"]=cfg.user;
  d["mpass"]=cfg.mpass;
  d["base"]=cfg.base;
  d["baud"]=cfg.baud;
  File f=LittleFS.open(CFG,"w");
  if(f) {
    serializeJson(d,f);
    f.close();
  }
}

void pub(const char*sub,double v,bool ret) {
  if(!mqtt.connected()) return;
  char tp[96],pl[24];
  snprintf(tp,sizeof tp,"%s/%s",cfg.base,sub);
  dtostrf(v,0,3,pl);
  mqtt.publish(tp,pl,ret);
}

// ---------- ZCL frame decode ----------
static uint8_t fr[256];
static size_t fl=0;
static bool inF=false,esc=false;
static const uint8_t TAG[4]       = {0x01, 0x09, 0x07, 0x02};
static const uint8_t TAG_PREPAY[4] = {0x01, 0x09, 0x07, 0x05};
static const uint8_t DEM[4]        = {0x00, 0x04, 0x00, 0x2A};
static const uint8_t SUM[4]        = {0x00, 0x00, 0x00, 0x25};
int fseq(const uint8_t*b,size_t n,const uint8_t*p) {
  if(n<4)return -1;
  for(size_t i=0;i+4<=n;i++) if(!memcmp(b+i,p,4)) return (int)i;
  return -1;
}
int32_t i24(const uint8_t*p) {
  int32_t v=p[0]|(p[1]<<8)|(p[2]<<16);
  if(v&0x800000)v-=(1<<24);
  return v;
}
uint64_t u48(const uint8_t*p) {
  uint64_t v=0;
  for(int i=5;i>=0;i--) v=(v<<8)|p[i];
  return v;
}

void handle(const uint8_t*f,size_t n) {
  if(n<6||f[0]!=0x80||f[1]!=0x02) return;
  int t=fseq(f,n,TAG);
  if(t<0||(size_t)(t+4)>=n) return;
  uint8_t ch=f[t+4];
  const char*nm=(ch==1)?"elec":(ch==2)?"gas":0;
  if(!nm) return;
  char sub[32];
  int d=fseq(f,n,DEM);
  if(d>=0&&(size_t)(d+7)<=n) {
    double w=i24(f+d+4);
    snprintf(sub,sizeof sub,"%s/live_power_w",nm);
    pub(sub,w,false);
    if(ch==1) {
      vElecW=w;
      tElecW=millis();
    }
  }
  int s=fseq(f,n,SUM);
  if(s>=0&&(size_t)(s+10)<=n) {
    double k=u48(f+s+4)/1000.0;
    snprintf(sub,sizeof sub,"%s/meter",nm);
    pub(sub,k,true);
    if(ch==1) {
      vElecK=k;
      tElecK=millis();
    } else  {
      vGasK=k;
      tGasK=millis();
    }
  }
}
void feed(uint8_t b) {
  if(esc) {
    if(inF&&fl<sizeof fr) fr[fl++]=b^0x08;
    esc=false;
    return;
  }
  if(b==0x01) {
    inF=true;
    fl=0;
    esc=false;
  }
  else if(b==0x03) {
    if(inF) handle(fr,fl);
    inF=false;
    fl=0;
    esc=false;
  }
  else if(b==0x02) {
    if(inF) esc=true;
  }
  else if(inF&&fl<sizeof fr) fr[fl++]=b;
}

// ---------- web UI ----------
String ageStr(unsigned long t) {
  if(!t) return "never";
  unsigned long s=(millis()-t)/1000;
  return String(s)+"s ago";
}
String numStr(double v,int dp) {
  if(isnan(v)) return "--";
  char b[24];
  dtostrf(v,0,dp,b);
  return b;
}
const char PAGE_HEAD[] PROGMEM =
"<!doctype html><title>geotrio2mqtt</title><meta name=viewport content='width=device-width,initial-scale=1'>"
"<style>body{font-family:system-ui,Arial;margin:0;background:#101317;color:#e8eaed}"
".w{max-width:620px;margin:0 auto;padding:18px}h1{font-size:19px}"
".card{background:#1a1f26;border:1px solid #2a313b;border-radius:12px;padding:16px;margin:12px 0}"
".units{font-size:22px}"
".big{font-size:28px;font-weight:600}.sub{color:#8a93a0;font-size:12px}"
"a{color:#6fb3ff}label{display:block;margin:8px 0 2px;font-size:13px;color:#b9c2cd}"
"input{width:100%;padding:8px;border-radius:8px;border:1px solid #2a313b;background:#0d1014;color:#e8eaed;box-sizing:border-box}"
"button{margin-top:14px;padding:10px 16px;border:0;border-radius:8px;background:#2f6fed;color:#fff;font-size:15px;cursor:pointer}"
".row{display:flex;gap:12px}.row>div{flex:1}</style>";
void sendLiveJson() {
  String j="{";
  j+="\"elecW\":"+numStr(vElecW,0)+",\"elecWage\":\""+ageStr(tElecW)+"\",";
  j+="\"elecK\":"+numStr(vElecK,3)+",\"elecKage\":\""+ageStr(tElecK)+"\",";
  j+="\"gasK\":"+numStr(vGasK,3)+",\"gasKage\":\""+ageStr(tGasK)+"\",";
  j+="\"rssi\":"+String(WiFi.RSSI())+",";
  j+="\"ssid\":\""+String(WiFi.status()==WL_CONNECTED?WiFi.SSID():"")+"\",";
  j+="\"mqtt\":"+String(mqtt.connected()?1:0)+",";
  j+="\"up\":"+String((millis()-bootMs)/1000)+",";
  j+="\"heap\":"+String(ESP.getFreeHeap())+",";
  j+="\"heapTotal\":"+String(ESP.getFreeHeap()+ESP.getMaxFreeBlockSize());
  j+="}";
  web.send(200,"application/json",j);
}
void handleRoot() {
  String h=FPSTR(PAGE_HEAD);
  h+="<div class=w><h1>geotrio2mqtt</h1>";
  h+="<div class=card><div class=sub>Electric &mdash; live power</div>"
  "<div class=big id=ew>---</div><div class=sub id=ewa></div></div>";
  h+="<div class=row>"
  "<div class=card><div class=sub>Electric meter</div><div class=big><span id=ek>---</span><span class=units> kWh</span></div>"
  "<div class=sub><span id=eka></span></div></div>"
  "<div class=card><div class=sub>Gas meter</div><div class=big><span id=gk>---</span><span class=units> m&#179;</span></div>"
  "<div class=sub><span id=gka></span></div></div></div>";
  h+="<div class=card><div class=sub>Status</div>"
  "<div id=st class=sub></div></div>";
  h+="<a href=/config><button type=button>Settings</button></a>";
  h+="<div class=sub style='text-align:center;margin-top:24px'>"
  "<a href='https://github.com/nadnerb33/geotrio2mqtt'>"
  "https://github.com/nadnerb33/geotrio2mqtt</a></div>";
  h+="<script>"
  "function fmtUp(s){"
  "if(s<60)return s.toLocaleString()+' sec(s)';"
  "if(s<3600)return Math.floor(s/60).toLocaleString()+' min(s)';"
  "if(s<86400)return Math.floor(s/3600).toLocaleString()+' hr(s)';"
  "return Math.floor(s/86400).toLocaleString()+' day(s)';"
  "}"
  "async function u(){try{"
  "let r=await fetch('/live');let d=await r.json();"
  "ew.textContent=d.elecW.toLocaleString()+' W';"
  "ewa.textContent=d.elecWage;"
  "ek.textContent=d.elecK.toLocaleString();"
  "eka.textContent=d.elecKage;"
  "gk.textContent=d.gasK.toLocaleString();"
  "gka.textContent=d.gasKage;"
  "let wifi=d.ssid?"
  "'WiFi: '+d.ssid+' ('+d.rssi+' dBm)'"
  ":'Access Point Mode';"
  "let mem=(d.heap/1024).toLocaleString(undefined,{maximumFractionDigits:1});"
  "st.textContent=wifi+' - MQTT '+(d.mqtt?'connected':'down')+"
  "' - up '+fmtUp(d.up)+' - Memory '+mem+' KB free';"
  "}catch(e){}}"
  "u();setInterval(u,2000);"
  "</script></div>";
  web.send(200,"text/html",h);
}
String field(const char*lbl,const char*name,const char*val,const char*type="text") {
  return "<label>"+String(lbl)+"</label><input name="+name+" type="+type+" value='"+String(val)+"'>";
}
void handleConfig() {
  String h=FPSTR(PAGE_HEAD);
  h+="<div class=w><h1>Settings</h1><form method=POST action=/save><div class=card>";
  h+="<div class=sub>WiFi</div>";
  h+=field("SSID","ssid",cfg.ssid)+field("Password","wpass",cfg.wpass,"password");
  h+="</div><div class=card><div class=sub>MQTT broker</div>";
  h+="<div class=row><div>"+field("Host","host",cfg.host)+"</div><div>"+field("Port","port",cfg.port)+"</div></div>";
  h+="<div class=row><div>"+field("User","user",cfg.user)+"</div><div>"+field("Password","mpass",cfg.mpass,"password")+"</div></div>";
  h+=field("Topic base","base",cfg.base);
  h+="</div><div class=card><div class=sub>Meter</div>"+field("Baud","baud",cfg.baud)+"</div>";
  h+="<button type=submit>Save &amp; reboot</button> &nbsp; <a href=/>Cancel</a></form></div>";
  web.send(200,"text/html",h);
}
void handleSave() {
  auto g=[&](const char*n,char*dst,size_t sz) {
    if(web.hasArg(n)) strlcpy(dst,web.arg(n).c_str(),sz);
  };
  g("ssid",cfg.ssid,sizeof cfg.ssid);
  g("wpass",cfg.wpass,sizeof cfg.wpass);
  g("host",cfg.host,sizeof cfg.host);
  g("port",cfg.port,sizeof cfg.port);
  g("user",cfg.user,sizeof cfg.user);
  g("mpass",cfg.mpass,sizeof cfg.mpass);
  g("base",cfg.base,sizeof cfg.base);
  g("baud",cfg.baud,sizeof cfg.baud);
  saveCfg();
  web.send(200,"text/html","<meta http-equiv=refresh content='4;url=/'>Saved. Rebooting...");
  delay(400);
  ESP.restart();
}
void startWeb() {
  web.on("/",handleRoot);
  web.on("/live",sendLiveJson);
  web.on("/config",handleConfig);
  web.on("/save",HTTP_POST,handleSave);
  web.onNotFound([](){ if(apMode){ web.sendHeader("Location","/"); web.send(302,"text/plain",""); }
  else web.send(404,"text/plain","404"); });
  web.begin();
}
void startAP() {
  apMode=true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP("geotrio2mqtt");
  dns.start(53,"*",WiFi.softAPIP());
}
void setup() {
  bootMs=millis();
  pinMode(0,INPUT_PULLUP);
  // FLASH button -> force setup AP at runtime
  snprintf(nodeId,sizeof nodeId,"geo%06x",ESP.getChipId());
  loadCfg();
  snprintf(statusTopic,sizeof statusTopic,"%s/status",cfg.base);
  Serial.begin(atoi(cfg.baud));
  Serial.swap();
  // UART0 RX -> GPIO13 (D7); meter wires here
  if(cfg.ssid[0]) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(cfg.ssid,cfg.wpass);
    for(int i=0;i<40 && WiFi.status()!=WL_CONNECTED;i++) delay(250);
  }
  if(WiFi.status()!=WL_CONNECTED) startAP();
  startWeb();
  mqtt.setServer(cfg.host,atoi(cfg.port));
}
void mqttTry() {
  String id=String(nodeId);
  if(mqtt.connect(id.c_str(), cfg.user[0]?cfg.user:0, cfg.user[0]?cfg.mpass:0,
  statusTopic,0,true,"offline")) {
    mqtt.publish(statusTopic,"online",true);
  }
}
void loop() {
  // hold FLASH (GPIO0) briefly at runtime to drop into the setup AP/portal
  if(digitalRead(0)==LOW) {
    delay(50);
    if(digitalRead(0)==LOW && !apMode) {
      startAP();
    }
  }
  if(apMode) {
    dns.processNextRequest();
    web.handleClient();
    while(Serial.available()) feed((uint8_t)Serial.read());
    return;
  }
  web.handleClient();
  if(WiFi.status()!=WL_CONNECTED) {
    WiFi.reconnect();
    delay(300);
    return;
  }
  if(!mqtt.connected()) {
    if(millis()-lastMqtt>3000) {
      lastMqtt=millis();
      mqttTry();
    }
  }
  else mqtt.loop();
  while(Serial.available()) feed((uint8_t)Serial.read());
}
