/*
 * geotrio2mqtt.ino - https://github.com/nadnerb33/geotrio2mqtt/
 * -------------------------------------------------------------------------
 * Geo Trio II smart meter display -> MQTT
 *
 * Works on ESP8266 (e.g. NodeMCU) and ESP32 (e.g. Seeed XIAO ESP32S3) boards.
 * This file is shared by all boards; board-specific code (libraries, meter RX
 * pin, debug log port) is in platform.h and is picked automatically from the
 * board selected in Tools > Board.
 *
 * See README.md for hardware, wiring, flashing, setup and MQTT topics.
 *
 * Libraries (Tools > Manage Libraries): ArduinoJson (>=7), PubSubClient (>=2.8)
 * (WiFi, web server, DNSServer, LittleFS and mDNS come with the board package.)
 * -------------------------------------------------------------------------
 */
#include <DNSServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <time.h>
#include "platform.h"

// ======================= settings you may want to change =======================

// Default network name (changeable on the Settings page): shown in your router
// and reachable as http://<name>.local
#define HOSTNAME "geotrio2mqtt"

// Meter scaling. The meter reports its own multiplier/divisor (ZCL attributes
// 0x0301/0x0302) when the Trio II starts up; geotrio2mqtt learns them, saves them
// and uses them from then on. These defaults are used until then.
// If your readings are 10x (or 100x) out, power-cycle the Trio II once while the
// ESP is running so it re-sends its scaling.
#define DEFAULT_METER_MULTIPLIER 1
#define DEFAULT_ELEC_DIVISOR 1000
#define DEFAULT_GAS_DIVISOR 1000
// Gas today/week/month usage (sent by the meter in kWh) is divided by this.
#define GAS_KWH_DIVISOR 1000.0

// Web page "Energy now" display (kW). These are the DEFAULTS - they can also be
// changed on the device's Settings page (Display section) without re-uploading.
// electricity dial needle reaches the end at this power (linear scale)
#define ELEC_DIAL_MAX_KW 15
// gas flame grows to full size at this power
#define GAS_FLAME_MAX_KW 25

// No change in gas kWh-today for this long = gas off (0 kW on the web page)
#define GAS_IDLE_MS (35UL*60000UL)

// DEBUG: 1 = log every meter frame the code ignores (as hex) to the debug log
// port (see platform.h). Useful for finding new data in the meter's messages.
#define DUMP_UNDECODED 0

// ===============================================================================

#define STR_(x) #x
#define STR(x) STR_(x)

struct Config {
  // Add your wifi details here
  char ssid[33] = "", wpass[65] = "";

  // Device network name (letters, numbers, hyphens)
  char name[33] = HOSTNAME;

  // MQTT broker details
  char host[40] = "192.168.0.33", port[6] = "1883", user[32] = "", mpass[32] = "";

  // MQTT topic base e.g <geotrio>/elec/live_power_w
  char base[48] = "geotrio";

  char baud[8] = "115200";

  // Web page display: full scale of the electricity dial and gas flame (kW)
  char emax[8] = STR(ELEC_DIAL_MAX_KW), gmax[8] = STR(GAS_FLAME_MAX_KW);
} cfg;
const char *CFG="/config.json";

WebServerT web(80);
DNSServer dns;
WiFiClient net;
PubSubClient mqtt(net);
bool apMode=false;
char statusTopic[64], nodeId[20];

// ---------- readings ----------
volatile double vElecW=NAN, vElecK=NAN, vGasK=NAN;
// usage in kWh: [0]=elec, [1]=gas; [0]=today, [1]=this week, [2]=this month
double vHist[2][3]={{NAN,NAN,NAN},{NAN,NAN,NAN}};
// cost in GBP incl. standing charge: same layout as vHist
double vCost[2][3]={{NAN,NAN,NAN},{NAN,NAN,NAN}};
// decimal places of cost values per fuel; updated from attr 0x0500 when the meter sends it
uint8_t costDp[2]={5,2};
unsigned long tElecW=0, tElecK=0, tGasK=0, bootMs=0, lastMqtt=0, lastWifi=0;

// Meter scaling per fuel ([0]=elec, [1]=gas): value = raw * mult / div
uint32_t mMult[2]={DEFAULT_METER_MULTIPLIER,DEFAULT_METER_MULTIPLIER};
uint32_t mDiv[2]={DEFAULT_ELEC_DIVISOR,DEFAULT_GAS_DIVISOR};
bool mLearned[2]={false,false};   // true = scaling came from the meter
const char *METERF="/meter.json";
double scale(int fi) { return (double)mMult[fi]/(double)mDiv[fi]; }

// Gas only updates every ~10-30 min, so the last gas values are kept in flash and
// shown straight after a restart. gasDirty = a gas value changed since the last save.
const char *GASF="/gas.json";
bool gasDirty=false, gasRestored=false;
unsigned long lastGasSave=0;
// real time (UTC seconds) the gas meter reading last changed; 0 = unknown. Survives restarts.
uint32_t gasEpoch=0;

// Gas "power now" (kW): the meter doesn't send it, so it's the average since the previous
// change in gas kWh-today (like the Trio display does). tGasChg = millis() of last change.
double vGasP=NAN;
unsigned long tGasChg=0;

// diagnostics: raw bytes seen on the meter UART, complete frames, frames we decoded
unsigned long rxBytes=0, rxFrames=0, rxDecoded=0;

// current UTC time in seconds, or 0 if the clock hasn't synced from the internet yet
uint32_t nowEpoch() {
  time_t t=time(nullptr);
  return t>1600000000 ? (uint32_t)t : 0;
}

// a display max setting in kW, falling back to the #define default if blank/invalid
double kwSetting(const char*s,double def) {
  double v=atof(s);
  return (v>0 && v<1000) ? v : def;
}

// ---------- config persistence ----------
void loadCfg() {
  if(!platformBeginFS()) return;
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
    strlcpy(cfg.name,d["name"]|cfg.name,sizeof cfg.name);
    strlcpy(cfg.emax,d["emax"]|cfg.emax,sizeof cfg.emax);
    strlcpy(cfg.gmax,d["gmax"]|cfg.gmax,sizeof cfg.gmax);
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
  d["name"]=cfg.name;
  d["emax"]=cfg.emax;
  d["gmax"]=cfg.gmax;
  File f=LittleFS.open(CFG,"w");
  if(f) {
    serializeJson(d,f);
    f.close();
  }
}

// ---------- meter scaling persistence ----------
void saveMeter() {
  JsonDocument d;
  for(int i=0;i<2;i++) {
    d["m"][i]=mMult[i];
    d["d"][i]=mDiv[i];
    d["l"][i]=mLearned[i];
  }
  File f=LittleFS.open(METERF,"w");
  if(f) {
    serializeJson(d,f);
    f.close();
  }
}

void loadMeter() {
  File f=LittleFS.open(METERF,"r");
  if(!f) return;
  JsonDocument d;
  if(!deserializeJson(d,f)) {
    for(int i=0;i<2;i++) {
      uint32_t m=d["m"][i]|0UL, v=d["d"][i]|0UL;
      if(m && v) {
        mMult[i]=m;
        mDiv[i]=v;
        mLearned[i]=d["l"][i]|false;
      }
    }
  }
  f.close();
}

// ---------- last gas values persistence ----------
void saveGas() {
  JsonDocument d;
  d["k"]=(double)vGasK;                  // NaN is written as null
  d["t"]=gasEpoch;                       // when it changed (UTC seconds, 0 = unknown)
  JsonArray h=d["h"].to<JsonArray>();
  JsonArray c=d["c"].to<JsonArray>();
  for(int i=0;i<3;i++) {
    h.add(vHist[1][i]);
    c.add(vCost[1][i]);
  }
  File f=LittleFS.open(GASF,"w");
  if(f) {
    serializeJson(d,f);
    f.close();
  }
}

void loadGas() {
  File f=LittleFS.open(GASF,"r");
  if(!f) return;
  JsonDocument d;
  if(!deserializeJson(d,f)) {
    vGasK=d["k"]|(double)NAN;
    gasEpoch=d["t"]|(uint32_t)0;
    for(int i=0;i<3;i++) {
      vHist[1][i]=d["h"][i]|(double)NAN;
      vCost[1][i]=d["c"][i]|(double)NAN;
    }
    gasRestored=true;
  }
  f.close();
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
static const uint8_t TAG[4]        = {0x01, 0x09, 0x07, 0x02};   // metering cluster 0x0702
static const uint8_t TAG_PREPAY[4] = {0x01, 0x09, 0x07, 0x05};   // prepayment cluster 0x0705
static const uint8_t DEM[4]        = {0x00, 0x04, 0x00, 0x2A};   // 0x0400 instantaneous demand
static const uint8_t SUM[4]        = {0x00, 0x00, 0x00, 0x25};   // 0x0000 summation delivered
static const uint8_t UOM[4]        = {0x00, 0x03, 0x00, 0x30};   // 0x0300 unit of measure
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

// Find a ZCL attribute record [id lo][id hi][status 00][type] with an unsigned
// integer type (0x20 uint8 .. 0x27 uint64) and return its little-endian value.
bool readUAttr(const uint8_t*f,size_t n,uint16_t id,uint64_t&out) {
  for(size_t i=0;i+4<=n;i++) {
    if(f[i]==(id&0xFF)&&f[i+1]==(id>>8)&&f[i+2]==0x00&&f[i+3]>=0x20&&f[i+3]<=0x27) {
      size_t len=f[i+3]-0x1F;
      if(i+4+len>n) return false;
      uint64_t v=0;
      for(int k=(int)len-1;k>=0;k--) v=(v<<8)|f[i+4+k];
      out=v;
      return true;
    }
  }
  return false;
}

// Prepayment cluster (0x0705) frames: cost of energy used today / this week / this month
// (attrs 0x051C / 0x0530 / 0x0540), including standing charge.
void handlePrepay(const uint8_t*f,size_t n) {
  int t=fseq(f,n,TAG_PREPAY);
  if(t<0||(size_t)(t+4)>=n) return;
  uint8_t ch=f[t+4];
  if(ch!=1&&ch!=2) return;
  rxDecoded++;
  int fi=ch-1;
  // attr 0x0500 (HistoricalCostConsumptionFormatting, bitmap8): low 3 bits = decimal places
  static const uint8_t FMT[4]={0x00,0x05,0x00,0x18};
  int p=fseq(f,n,FMT);
  if(p>=0&&(size_t)(p+5)<=n) costDp[fi]=f[p+4]&0x07;
  double div=pow(10,costDp[fi]);
  static const uint16_t CID[3]={0x051C,0x0530,0x0540};
  static const char*CNM[3]={"cost_today","cost_week","cost_month"};
  char sub[32];
  for(int i=0;i<3;i++) {
    uint64_t raw;
    if(readUAttr(f,n,CID[i],raw)) {
      double v=raw/div;
      if(fi==1 && v!=vCost[fi][i]) gasDirty=true;
      vCost[fi][i]=v;
      snprintf(sub,sizeof sub,"%s/%s",ch==1?"elec":"gas",CNM[i]);
      pub(sub,v,true);
    }
  }
}

// Metering cluster (0x0702) frame carrying the meter's formatting attributes
// (sent when the Trio II starts up): learn multiplier 0x0301 / divisor 0x0302.
void learnScaling(const uint8_t*f,size_t n,int fi) {
  if(fseq(f,n,UOM)<0) return;
  uint64_t m=0,d=0;
  bool changed=false;
  if(readUAttr(f,n,0x0302,d) && d>0 && d<=0xFFFFFF && (d!=mDiv[fi] || !mLearned[fi])) {
    mDiv[fi]=(uint32_t)d;
    changed=true;
  }
  if(readUAttr(f,n,0x0301,m) && m>0 && m<=0xFFFFFF && (m!=mMult[fi] || !mLearned[fi])) {
    mMult[fi]=(uint32_t)m;
    changed=true;
  }
  if(changed) {
    mLearned[fi]=true;
    saveMeter();
    LOG.printf("%s scaling from meter: x%lu / %lu\n",fi?"gas":"elec",
      (unsigned long)mMult[fi],(unsigned long)mDiv[fi]);
  }
}

void handle(const uint8_t*f,size_t n) {
  if(n<6||f[0]!=0x80||f[1]!=0x02) return;
  int t=fseq(f,n,TAG);
  if(t<0) {
    handlePrepay(f,n);
    return;
  }
  if((size_t)(t+4)>=n) return;
  uint8_t ch=f[t+4];
  const char*nm=(ch==1)?"elec":(ch==2)?"gas":0;
  if(!nm) return;
  rxDecoded++;
  int fi=ch-1;
  learnScaling(f,n,fi);
  char sub[32];
  int d=fseq(f,n,DEM);
  if(d>=0&&(size_t)(d+7)<=n) {
    // demand is in kW after scaling; published in W
    double w=i24(f+d+4);
    if(ch==1) w*=scale(0)*1000.0;
    snprintf(sub,sizeof sub,"%s/live_power_w",nm);
    pub(sub,w,false);
    if(ch==1) {
      vElecW=w;
      tElecW=millis();
    }
  }
  int s=fseq(f,n,SUM);
  if(s>=0&&(size_t)(s+10)<=n) {
    double k=u48(f+s+4)*scale(fi);   // kWh (elec) or m3 (gas)
    snprintf(sub,sizeof sub,"%s/meter",nm);
    pub(sub,k,true);
    if(ch==1) {
      vElecK=k;
      tElecK=millis();
    } else  {
      if(k!=vGasK) {
        gasDirty=true;
        gasEpoch=nowEpoch();   // real time the reading changed (0 if clock not synced yet)
      }
      vGasK=k;
      tGasK=millis();
    }
  }
  // Today / this week / this month consumption.
  // Elec: Historical Consumption attrs 0x0401/0x0430/0x0440 (meter units -> kWh via scaling)
  // Gas:  Alternative Historical Consumption attrs 0x0C01/0x0C30/0x0C40 (kWh, /GAS_KWH_DIVISOR)
  static const uint16_t HID[2][3]={{0x0401,0x0430,0x0440},{0x0C01,0x0C30,0x0C40}};
  static const char*HNM[3]={"today_kwh","week_kwh","month_kwh"};
  for(int i=0;i<3;i++) {
    uint64_t raw;
    if(readUAttr(f,n,HID[fi][i],raw)) {
      double v=(ch==1) ? raw*scale(0) : raw/GAS_KWH_DIVISOR;
      if(fi==1 && v!=vHist[fi][i]) {
        gasDirty=true;
        // gas today changed: average kW since the previous change seen this session
        if(i==0) {
          double prev=vHist[1][0];
          unsigned long nowMs=millis();
          if(tGasChg && !isnan(prev) && v>=prev) {
            double hrs=(nowMs-tGasChg)/3600000.0;
            if(hrs>0) vGasP=(v-prev)/hrs;
          }
          tGasChg=nowMs;   // first change (or midnight reset) just sets the baseline
        }
      }
      vHist[fi][i]=v;
      snprintf(sub,sizeof sub,"%s/%s",nm,HNM[i]);
      pub(sub,v,true);
    }
  }
}

void feed(uint8_t b) {
  rxBytes++;
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
    if(inF) {
      rxFrames++;
      unsigned long before=rxDecoded;
      handle(fr,fl);
#if DUMP_UNDECODED
      // only frames we did NOT decode, skipping the frequent 7F FF / 80 08 / 80 09
      // housekeeping traffic. Prefixed with uptime in seconds to see when they arrived.
      bool housekeeping=fl>=2 && ((fr[0]==0x7F && fr[1]==0xFF) ||
                                  (fr[0]==0x80 && (fr[1]==0x08 || fr[1]==0x09)));
      if(rxDecoded==before && !housekeeping) {
        LOG.printf("[up %lus] IGNORED len=%u:",millis()/1000,(unsigned)fl);
        for(size_t i=0;i<fl;i++) LOG.printf(" %02X",fr[i]);
        LOG.println();
      }
#else
      (void)before;
#endif
    }
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
// human-friendly age, e.g. "12s ago", "25 min ago", "2 h ago", "3 days ago"
String fmtAge(unsigned long s) {
  if(s<60) return String(s)+"s ago";
  if(s<3600) return String(s/60)+" min ago";
  if(s<86400) return String(s/3600)+" h ago";
  return String(s/86400)+(s<172800?" day ago":" days ago");
}
String ageStr(unsigned long t) {
  if(!t) return "never";
  return fmtAge((millis()-t)/1000);
}
String numStr(double v,int dp) {
  if(isnan(v)) return "null";   // must be valid JSON, or the page's fetch fails
  char b[24];
  dtostrf(v,0,dp,b);
  return b;
}
String scaleStr(int fi) {
  String s=(fi?"gas ":"elec ");
  if(mMult[fi]!=1) s+="x"+String(mMult[fi])+" ";
  s+="/"+String(mDiv[fi])+(mLearned[fi]?" (from meter)":" (default)");
  return s;
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
  // After a restart, the saved gas reading's age comes from its saved real-time
  // timestamp; fall back to a label if the clock hasn't synced yet.
  String gasAge;
  if(tGasK) gasAge=ageStr(tGasK);
  else if(gasRestored && !isnan(vGasK)) {
    uint32_t now=nowEpoch();
    if(now && gasEpoch && now>=gasEpoch) gasAge=fmtAge(now-gasEpoch);
    else gasAge="from before restart";
  }
  else gasAge="never";
  j+="\"gasK\":"+numStr(vGasK,3)+",\"gasKage\":\""+gasAge+"\",";
  // gas power now (kW): 0 if gas kWh hasn't moved for GAS_IDLE_MS
  double gp=vGasP;
  if(!isnan(gp) && millis()-tGasChg>GAS_IDLE_MS) gp=0;
  j+="\"gasP\":"+numStr(gp,2)+",\"gasPage\":\""+ageStr(tGasChg)+"\",";
  // h: [[elec today,week,month],[gas today,week,month]] in kWh
  j+="\"h\":[";
  for(int a=0;a<2;a++) {
    j+=(a?",[":"[");
    for(int b=0;b<3;b++) {
      if(b) j+=",";
      j+=numStr(vHist[a][b],3);
    }
    j+="]";
  }
  j+="],";
  // c: same layout as h, cost in GBP
  j+="\"c\":[";
  for(int a=0;a<2;a++) {
    j+=(a?",[":"[");
    for(int b=0;b<3;b++) {
      if(b) j+=",";
      j+=numStr(vCost[a][b],2);
    }
    j+="]";
  }
  j+="],";
  j+="\"scale\":\""+scaleStr(0)+", "+scaleStr(1)+"\",";
  j+="\"rssi\":"+String(WiFi.RSSI())+",";
  j+="\"ssid\":\""+String(WiFi.status()==WL_CONNECTED?WiFi.SSID():"")+"\",";
  j+="\"mqtt\":"+String(mqtt.connected()?1:0)+",";
  j+="\"up\":"+String((millis()-bootMs)/1000)+",";
  j+="\"heap\":"+String(ESP.getFreeHeap())+",";
  j+="\"rxB\":"+String(rxBytes)+",\"rxF\":"+String(rxFrames)+",\"rxD\":"+String(rxDecoded)+",";
  j+="\"heapTotal\":"+String(platformHeapTotal());
  j+="}";
  web.send(200,"application/json",j);
}
void handleRoot() {
  String h=FPSTR(PAGE_HEAD);
  h+="<div class=w><h1>geotrio2mqtt</h1>";
  // "Energy now": electricity dial + gas flame, like the Trio II display
  h+="<div class=row>"
  "<div class=card style='text-align:center'><div class=sub>Electricity now</div>"
  "<svg viewBox='0 0 200 170' style='width:100%;max-width:230px'>"
  "<g id=arc></g>"
  "<g id=ndl style='transform-origin:100px 100px;transform:rotate(135deg);transition:transform .8s'>"
  "<line x1=100 y1=100 x2=168 y2=100 stroke='#d6ecff' stroke-width=6 stroke-linecap=round /></g>"
  "<circle cx=100 cy=100 r=12 fill='#6fb3ff' stroke='#1a1f26' stroke-width=3 /></svg>"
  "<div class=big><span id=ew>---</span><span class=units> kW</span></div>"
  "<div class=sub id=ewa></div></div>"
  "<div class=card style='text-align:center'><div class=sub>Gas now</div>"
  "<svg viewBox='0 0 200 170' style='width:100%;max-width:230px'>"
  "<defs><linearGradient id=fg x1=0 y1=1 x2=0 y2=0>"
  "<stop offset=0 stop-color='#1e4fd8'/><stop offset=1 stop-color='#5aa0ff'/></linearGradient></defs>"
  "<g id=flm style='transform-origin:100px 162px;opacity:.35;filter:grayscale(1);"
  "transition:opacity .8s,filter .8s,transform .8s'>"
  "<path fill='url(#fg)' d='M100 10C112 45 150 70 150 112C150 142 128 162 100 162C72 162 50 142 50 112"
  "C50 88 64 70 76 52C78 74 86 86 96 92C92 64 94 36 100 10Z'/>"
  "<path fill='#a8d4ff' opacity=.75 d='M102 70C108 92 126 104 126 126C126 144 115 154 100 154"
  "C85 154 74 144 74 128C74 114 82 104 88 96C90 110 95 118 101 121C98 104 99 88 102 70Z'/></g></svg>"
  "<div class=big><span id=gw>---</span><span class=units> kW</span></div>"
  "<div class=sub id=gwa></div></div></div>";
  h+="<div class=row>"
  "<div class=card><div class=sub>Electric meter</div><div class=big><span id=ek>---</span><span class=units> kWh</span></div>"
  "<div class=sub><span id=eka></span></div></div>"
  "<div class=card><div class=sub>Gas meter</div><div class=big><span id=gk>---</span><span class=units> m&#179;</span></div>"
  "<div class=sub><span id=gka></span></div></div></div>";
  h+="<div class=card><div class=sub>Usage (kWh)</div>"
  "<table style='width:100%;text-align:right;font-size:18px;margin-top:6px'>"
  "<tr class=sub><th></th><th>Today</th><th>Week</th><th>Month</th></tr>"
  "<tr><th style='text-align:left'>Elec</th><td id=hED>---</td><td id=hEW>---</td><td id=hEM>---</td></tr>"
  "<tr><th style='text-align:left'>Gas</th><td id=hGD>---</td><td id=hGW>---</td><td id=hGM>---</td></tr>"
  "</table></div>";
  h+="<div class=card><div class=sub>Cost (&pound;, incl. standing charge)</div>"
  "<table style='width:100%;text-align:right;font-size:18px;margin-top:6px'>"
  "<tr class=sub><th></th><th>Today</th><th>Week</th><th>Month</th></tr>"
  "<tr><th style='text-align:left'>Elec</th><td id=cED>---</td><td id=cEW>---</td><td id=cEM>---</td></tr>"
  "<tr><th style='text-align:left'>Gas</th><td id=cGD>---</td><td id=cGW>---</td><td id=cGM>---</td></tr>"
  "</table></div>";
  h+="<div class=card><div class=sub>Status</div>"
  "<div id=st class=sub></div></div>";
  h+="<a href=/config><button type=button>Settings</button></a>";
  h+="<div class=sub style='text-align:center;margin-top:24px'>"
  "<a href='https://github.com/nadnerb33/geotrio2mqtt'>"
  "https://github.com/nadnerb33/geotrio2mqtt</a></div>";
  h+="<script>"
  // draw the dial arc once: 270 degree sweep, green -> red
  "(()=>{const N=36,r=78,P=a=>{a*=Math.PI/180;"
  "return (100+r*Math.cos(a)).toFixed(1)+' '+(100+r*Math.sin(a)).toFixed(1)};let s='';"
  "for(let i=0;i<N;i++){const a0=135+270*i/N,a1=Math.min(135+270*(i+1)/N+0.6,405);"
  "s+=`<path d='M${P(a0)} A${r} ${r} 0 0 1 ${P(a1)}' fill='none' stroke-width='16' "
  "stroke='hsl(${120-120*i/(N-1)},75%,48%)'/>`}"
  "document.getElementById('arc').innerHTML=s})();";
  // dial / flame full scale from Settings, defaulting to the #defines (EMAX in W, GMAX in kW)
  h+="const EMAX="+String(kwSetting(cfg.emax,ELEC_DIAL_MAX_KW)*1000,0)
     +",GMAX="+String(kwSetting(cfg.gmax,GAS_FLAME_MAX_KW),2)+";";
  h+="function fmtUp(s){"
  "if(s<60)return s.toLocaleString()+' sec(s)';"
  "if(s<3600)return Math.floor(s/60).toLocaleString()+' min(s)';"
  "if(s<86400)return Math.floor(s/3600).toLocaleString()+' hr(s)';"
  "return Math.floor(s/86400).toLocaleString()+' day(s)';"
  "}"
  "async function u(){try{"
  "let r=await fetch('/live');let d=await r.json();"
  "const n=v=>v==null?'---':v.toLocaleString();"
  // electricity dial (linear scale, 0..EMAX) + exact figure in kW
  "if(d.elecW!=null){"
  "const f=Math.min(Math.max(d.elecW,0),EMAX)/EMAX;"
  "ndl.style.transform='rotate('+(135+270*f)+'deg)';"
  "ew.textContent=(d.elecW/1000).toLocaleString(undefined,{minimumFractionDigits:3,maximumFractionDigits:3});}"
  "ewa.textContent=d.elecWage;"
  // gas flame: bright and bigger when gas is in use, grey when idle/unknown
  "const gp=d.gasP,on=gp!=null&&gp>0.05;"
  "gw.textContent=gp==null?'---':gp.toLocaleString(undefined,{minimumFractionDigits:2,maximumFractionDigits:2});"
  "flm.style.opacity=on?1:.35;flm.style.filter=on?'none':'grayscale(1)';"
  "flm.style.transform='scale('+(on?0.75+0.25*Math.min(gp/GMAX,1):0.75)+')';"
  "gwa.textContent=gp==null?'waiting for gas updates':'average, updated '+d.gasPage;"
  "ek.textContent=n(d.elecK);"
  "eka.textContent=d.elecKage;"
  "gk.textContent=n(d.gasK);"
  "gka.textContent=d.gasKage;"
  "const n1=v=>v==null?'---':v.toLocaleString(undefined,{maximumFractionDigits:1});"
  "[[hED,hEW,hEM],[hGD,hGW,hGM]].forEach((r,a)=>r.forEach((c,b)=>c.textContent=n1(d.h[a][b])));"
  "const n2=v=>v==null?'---':v.toLocaleString(undefined,{minimumFractionDigits:2,maximumFractionDigits:2});"
  "[[cED,cEW,cEM],[cGD,cGW,cGM]].forEach((r,a)=>r.forEach((c,b)=>c.textContent=n2(d.c[a][b])));"
  "let wifi=d.ssid?"
  "'WiFi: '+d.ssid+' ('+d.rssi+' dBm)'"
  ":'Access Point Mode';"
  "let mem=(d.heap/1024).toLocaleString(undefined,{maximumFractionDigits:1});"
  "st.textContent=wifi+' - MQTT '+(d.mqtt?'connected':'down')+"
  "' - up '+fmtUp(d.up)+' - Memory '+mem+' KB free'+"
  "' - Meter RX: '+d.rxB+' bytes, '+d.rxF+' frames, '+d.rxD+' decoded'+"
  "' - Scaling: '+d.scale;"
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
  h+="<div class=w><h1>Settings</h1><form method=POST action=/save>";
  h+="<div class=card><div class=sub>Display</div>";
  h+="<div class=row><div>"+field("Electricity dial max (kW)","emax",cfg.emax)+"</div>"
     "<div>"+field("Gas flame max (kW)","gmax",cfg.gmax)+"</div></div>";
  h+="<div class=sub style='margin-top:6px'>Dial: the needle reaches the end at this power. "
     "Flame: grows to full size at this power.</div>";
  h+="</div><div class=card>";
  h+="<div class=sub>WiFi</div>";
  h+=field("SSID","ssid",cfg.ssid)+field("Password","wpass",cfg.wpass,"password");
  h+=field("Device name (letters, numbers, hyphens)","name",cfg.name);
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
  g("name",cfg.name,sizeof cfg.name);
  g("emax",cfg.emax,sizeof cfg.emax);
  g("gmax",cfg.gmax,sizeof cfg.gmax);
  // network names may only contain a-z, 0-9 and '-', and can't start or end with '-'
  size_t o=0;
  for(size_t i=0;cfg.name[i];i++) {
    char c=tolower((unsigned char)cfg.name[i]);
    if(!isalnum((unsigned char)c)) c='-';
    if(c=='-' && (o==0 || cfg.name[o-1]=='-')) continue;   // no leading/double hyphens
    cfg.name[o++]=c;
  }
  while(o>0 && cfg.name[o-1]=='-') o--;                    // no trailing hyphen
  cfg.name[o]=0;
  if(!o) strlcpy(cfg.name,HOSTNAME,sizeof cfg.name);
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
  pinMode(0,INPUT_PULLUP);   // FLASH/BOOT button -> force setup AP at runtime
  snprintf(nodeId,sizeof nodeId,"geo%06x",(unsigned)(platformChipId()&0xFFFFFF));
  platformBeginLog();
  loadCfg();
  loadGas();
  loadMeter();
  snprintf(statusTopic,sizeof statusTopic,"%s/status",cfg.base);
  platformBeginMeter(atol(cfg.baud));
  LOG.printf("geotrio2mqtt node %s, meter RX on %s @ %s baud\n",nodeId,platformMeterPin().c_str(),cfg.baud);
  LOG.printf("scaling: %s, %s\n",scaleStr(0).c_str(),scaleStr(1).c_str());
  if(cfg.ssid[0]) {
    platformSetHostname(cfg.name);   // must be set before the WiFi starts (DHCP sends this name)
    WiFi.mode(WIFI_STA);
    WiFi.begin(cfg.ssid,cfg.wpass);
    for(int i=0;i<40 && WiFi.status()!=WL_CONNECTED;i++) delay(250);
    // internet time (UTC), used to timestamp the saved gas reading
    configTime(0,0,"pool.ntp.org","time.google.com");
  }
  if(WiFi.status()!=WL_CONNECTED) {
    LOG.println("WiFi not connected - starting AP 'geotrio2mqtt' (192.168.4.1)");
    startAP();
  } else {
    LOG.print("WiFi connected, IP ");
    LOG.println(WiFi.localIP());
    LOG.print("MAC ");
    LOG.println(WiFi.macAddress());
    if(MDNS.begin(cfg.name)) {
      MDNS.addService("http","tcp",80);
      LOG.printf("Web page: http://%s.local\n",cfg.name);
    }
  }
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
  // hold FLASH/BOOT (GPIO0) briefly at runtime to drop into the setup AP/portal
  if(digitalRead(0)==LOW) {
    delay(50);
    if(digitalRead(0)==LOW && !apMode) {
      startAP();
    }
  }
  platformLoop();
#if DUMP_UNDECODED
  // every 10s, log meter RX counters
  static unsigned long lastDiag=0;
  if(millis()-lastDiag>10000) {
    lastDiag=millis();
    LOG.printf("meter RX: %lu bytes, %lu frames, %lu decoded | elecW=%.0f elecK=%.3f gasK=%.3f\n",
      rxBytes,rxFrames,rxDecoded,(double)vElecW,(double)vElecK,(double)vGasK);
  }
#endif
  // save changed gas values, at most once a minute (limits flash wear)
  if(gasDirty && millis()-lastGasSave>60000) {
    lastGasSave=millis();
    gasDirty=false;
    saveGas();
  }
  if(apMode) {
    dns.processNextRequest();
    web.handleClient();
    while(MeterSerial.available()) feed((uint8_t)MeterSerial.read());
    return;
  }
  web.handleClient();
  if(WiFi.status()!=WL_CONNECTED) {
    // the WiFi stack reconnects by itself; only nudge it occasionally
    // (calling reconnect() too often can stop it ever connecting)
    if(millis()-lastWifi>10000) {
      lastWifi=millis();
      WiFi.reconnect();
    }
    while(MeterSerial.available()) feed((uint8_t)MeterSerial.read());
    delay(10);
    return;
  }
  if(!mqtt.connected()) {
    if(millis()-lastMqtt>3000) {
      lastMqtt=millis();
      mqttTry();
    }
  }
  else mqtt.loop();
  while(MeterSerial.available()) feed((uint8_t)MeterSerial.read());
}
