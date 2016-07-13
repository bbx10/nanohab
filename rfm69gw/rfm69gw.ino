/* RFM69 library and code by Felix Rusu - felix@lowpowerlab.com
// Get libraries at: https://github.com/LowPowerLab/
// Make sure you adjust the settings in the configuration section below !!!
// **********************************************************************************
// Copyright Felix Rusu, LowPowerLab.com
// Library and code by Felix Rusu - felix@lowpowerlab.com
// **********************************************************************************
// License
// **********************************************************************************
// This program is free software; you can redistribute it 
// and/or modify it under the terms of the GNU General    
// Public License as published by the Free Software       
// Foundation; either version 3 of the License, or        
// (at your option) any later version.                    
//                                                        
// This program is distributed in the hope that it will   
// be useful, but WITHOUT ANY WARRANTY; without even the  
// implied warranty of MERCHANTABILITY or FITNESS FOR A   
// PARTICULAR PURPOSE. See the GNU General Public        
// License for more details.                              
//                                                        
// You should have received a copy of the GNU General    
// Public License along with this program.
// If not, see <http://www.gnu.org/licenses></http:>.
//                                                        
// Licence can be viewed at                               
// http://www.gnu.org/licenses/gpl-3.0.txt
//
// Please maintain this license information along with authorship
// and copyright notices in any redistribution of this code
// **********************************************************************************/

#define SERIAL_BAUD   115200

#include <ESP8266WiFi.h>
#include <RFM69.h>                //https://www.github.com/lowpowerlab/rfm69
#include <pgmspace.h>

char RadioConfig[128];

// Default values
const char PROGMEM ENCRYPTKEY[] = "sampleEncryptKey";
const char PROGMEM MDNS_NAME[] = "rfm69gw1";
const char PROGMEM MQTT_BROKER[] = "raspi2";
const char PROGMEM RFM69AP_NAME[] = "RFM69-AP";
#define NETWORKID     200  //the same on all nodes that talk to each other
#define NODEID        1

//Match frequency to the hardware version of the radio
//#define FREQUENCY     RF69_433MHZ
//#define FREQUENCY     RF69_868MHZ
#define FREQUENCY      RF69_915MHZ
#define IS_RFM69HCW    true // set to 'true' if you are using an RFM69HCW module
#define POWER_LEVEL    31

// vvvvvvvvv Global Configuration vvvvvvvvvvv
#include <EEPROM.h>

struct _GLOBAL_CONFIG {
  uint32_t    checksum;
  char        rfmapname[32];
  char        mqttbroker[32];
  char        mqttclientname[32];
  char        mdnsname[32];
  uint32_t    ipaddress;  // if 0, use DHCP
  uint32_t    ipnetmask;
  uint32_t    ipgateway;
  uint32_t    ipdns1;
  uint32_t    ipdns2;
  char        encryptkey[16+1];
  uint8_t     networkid;
  uint8_t     nodeid;
  uint8_t     powerlevel; // bits 0..4 power leve, bit 7 RFM69HCW 1=true
  uint8_t     rfmfrequency;
};

#define GC_POWER_LEVEL    (pGC->powerlevel & 0x1F)
#define GC_IS_RFM69HCW  ((pGC->powerlevel & 0x80) != 0)

struct _GLOBAL_CONFIG *pGC;
// ^^^^^^^^^ Global Configuration ^^^^^^^^^^^

// vvvvvvvvv ESP8266 WiFi vvvvvvvvvvv
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>          //https://github.com/tzapu/WiFiManager

void configModeCallback (WiFiManager *myWiFiManager) {
  Serial.println("Entered config mode");
  Serial.println(WiFi.softAPIP());
  //if you used auto generated SSID, print it
  Serial.println(myWiFiManager->getConfigPortalSSID());
}

void wifi_setup(void) {
  //WiFiManager
  //Local intialization. Once its business is done, there is no need to keep it around
  WiFiManager wifiManager;
  //reset settings - for testing. Wipes out SSID/password.
  //wifiManager.resetSettings();

  //set callback that gets called when connecting to previous WiFi fails, and enters Access Point mode
  wifiManager.setAPCallback(configModeCallback);

  //fetches ssid and pass and tries to connect
  //if it does not connect it starts an access point with the specified name
  //here  "AutoConnectAP"
  //and goes into a blocking loop awaiting configuration
  if(!wifiManager.autoConnect(pGC->rfmapname)) {
    Serial.println("failed to connect and hit timeout");
    //reset and try again, or maybe put it to deep sleep
    ESP.reset();
    delay(1000);
  }

  Serial.println("connected");
}

// ^^^^^^^^^ ESP8266 WiFi ^^^^^^^^^^^

// vvvvvvvvv Global Configuration vvvvvvvvvvv
uint32_t gc_checksum() {
  uint8_t *p = (uint8_t *)pGC;
  uint32_t checksum = 0;
  p += sizeof(pGC->checksum);
  for (size_t i = 0; i < (sizeof(*pGC) - 4); i++) {
    checksum += *p++;
  }
  return checksum;
}

void eeprom_setup() {
  EEPROM.begin(4096);
  pGC = (struct _GLOBAL_CONFIG *)EEPROM.getDataPtr();
  // if checksum bad init GC else use GC values
  if (gc_checksum() != pGC->checksum) {
    Serial.println("Factory reset");
    memset(pGC, 0, sizeof(*pGC));
    strcpy_P(pGC->encryptkey, ENCRYPTKEY);
    strcpy_P(pGC->rfmapname, RFM69AP_NAME);
    strcpy_P(pGC->mqttbroker, MQTT_BROKER);
    strcpy_P(pGC->mdnsname, MDNS_NAME);
    strcpy(pGC->mqttclientname, WiFi.hostname().c_str());
    pGC->networkid = NETWORKID;
    pGC->nodeid = NODEID;
    pGC->powerlevel = ((IS_RFM69HCW)?0x80:0x00) | POWER_LEVEL;
    pGC->rfmfrequency = FREQUENCY;
    pGC->checksum = gc_checksum();
    EEPROM.commit();
  }
}
// ^^^^^^^^^ Global Configuration ^^^^^^^^^^^

// vvvvvvvvv ESP8266 web sockets vvvvvvvvvvv
#include <ESP8266mDNS.h>

// URL: http://rfm69gw.local
MDNSResponder mdns;

void mdns_setup(void) {
  if (pGC->mdnsname[0] == '\0') return;

  if (mdns.begin(pGC->mdnsname, WiFi.localIP())) {
    Serial.println("MDNS responder started");
    mdns.addService("http", "tcp", 80);
    mdns.addService("ws", "tcp", 81);
  }
  else {
    Serial.println("MDNS.begin failed");
  }
  Serial.printf("Connect to http://%s.local or http://", pGC->mdnsname);
  Serial.println(WiFi.localIP());
}

static const char PROGMEM INDEX_HTML[] = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta name = "viewport" content = "width = device-width, initial-scale = 1.0, maximum-scale = 1.0, user-scalable=0">
<title>RFM69 Gateway</title>
<style>
"body { background-color: #808080; font-family: Arial, Helvetica, Sans-Serif; Color: #000000; }"
</style>
<script>
var websock;
function start() {
  var rfm69nodes = [];
  websock = new WebSocket('ws://' + window.location.hostname + ':81/');
  websock.onopen = function(evt) { console.log('websock open'); };
  websock.onclose = function(evt) { console.log('websock close'); };
  websock.onerror = function(evt) { console.log(evt); };
  websock.onmessage = function(evt) {
    //console.log(evt);
    // evt.data holds the gateway radio status info in JSON format
    var gwobj = JSON.parse(evt.data);
    if (gwobj && gwobj.msgType) {
      if (gwobj.msgType === "config") {
        var eConfig = document.getElementById('rfm69Config');
        eConfig.innerHTML = '<p>Frequency ' + gwobj.freq + ' MHz' +
          ', Network ID:' + gwobj.netid +
          ', RFM69HCW:' + gwobj.rfm69hcw +
          ', Power Level:' + gwobj.power;
      }
      else if (gwobj.msgType === "status") {
        var eStatus = document.getElementById('rfm69Status');
        rfm69nodes[gwobj.senderId] = gwobj;
        var aTable = '<table>';
        aTable = aTable.concat(
            '<tr>' +
            '<th>Node</th>' +
            '<th>RSSI</th>' +
            '<th>Packets</th>' +
            '<th>Miss</th>' +
            '<th>Dup</th>' +
            '<th>Last</th>' +
            '</tr>');
        for (var i = 0; i <= 255; i++) {
          if (rfm69nodes[i]) {
            aTable = aTable.concat('<tr>' +
                '<td>' + rfm69nodes[i].senderId + '</td>' +
                '<td>' + rfm69nodes[i].rssi + '</td>' +
                '<td>' + rfm69nodes[i].rxMsgCnt  + '</td>' +
                '<td>' + rfm69nodes[i].rxMsgMiss + '</td>' +
                '<td>' + rfm69nodes[i].rxMsgDup  + '</td>' +
                '<td>' + rfm69nodes[i].message + '</td>' +
                '</tr>');
          }
        }
        aTable = aTable.concat('</table>');
        eStatus.innerHTML = aTable;
      }
      else {
      }
    }
  };
}
</script>
</head>
<body onload="javascript:start();">
<h2>RFM69 Gateway</h2>
<div id="rfm69Config"></div>
<div id="rfm69Status">Waiting for node data</div>
<div id="configureGateway">
  <p><a href="/configGW"><button type="button">Configure Gateway</button></a>
</div>
<div id="FirmwareUpdate">
  <p><a href="/updater"><button type="button">Update Gateway Firmware</button></a>
</div>
</body>
</html>
)rawliteral";

static const char PROGMEM CONFIGUREGW_HTML[] = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name = "viewport" content = "width = device-width, initial-scale = 1.0, maximum-scale = 1.0, user-scalable=0">
  <title>RFM69 Gateway Configuration</title>
  <style>
    "body { background-color: #808080; font-family: Arial, Helvetica, Sans-Serif; Color: #000000; }"
  </style>
</head>
<body>
  <h2>RFM69 Gateway Configuration</h2>
  <a href="/configGWrfm69"><button type="button">RFM69</button></a>
  <p>
  <a href="/configGWmqtt"><button type="button">MQTT</button></a>
  <p>
  <a href="/"><button type="button">Home</button></a>
</body>
</html>
)rawliteral";

static const char PROGMEM CONFIGUREGWRFM69_HTML[] = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name = "viewport" content = "width = device-width, initial-scale = 1.0, maximum-scale = 1.0, user-scalable=0">
  <title>RFM69 Gateway Configuration</title>
  <style>
    "body { background-color: #808080; font-family: Arial, Helvetica, Sans-Serif; Color: #000000; }"
  </style>
</head>
<body>
  <h3>RFM69 Gateway Configuration</h3>
  <form method='POST' action='/configGWrfm69' enctype='multipart/form-data'>
    <label>RFM69 Network ID</label>
    <input type='number' name='networkid' value="%d" min="1" max="255" size="3"><br>
    <label>RFM69 Node ID</label>
    <input type='number' name='nodeid' value="%d" min="1" max="255" size="3"><br>
    <label>RFM69 Encryption Key</label>
    <input type='text' name='encryptkey' value="%s" size="16" maxlength="16"><br>
    <label>RFM69 Power Level</label>
    <input type='number' name='powerlevel' value="%d" min="0" max="31"size="2"><br>
    <label>RFM69 Frequency</label>
    <select name="rfmfrequency">
    <option value="31" %s>315 MHz</option>
    <option value="43" %s>433 MHz</option>
    <option value="86" %s>868 MHz</option>
    <option value="91" %s>915 MHz</option>
    </select><br>
    <label for=hcw>RFM69 HCW</label><br>
    <input type='radio' name='rfm69hcw' id="hcw" value="1" %s> True<br>
    <input type='radio' name='rfm69hcw' id="hcw" value="0" %s> False<br>
    <label>RFM69 AP name</label>
    <input type='text' name='rfmapname' value="%s" size="32" maxlength="32"><br>
    <p><input type='submit' value='Save changes'>
  </form>
  <p><a href="/configGW"><button type="button">Cancel</button></a><a href="/configGWreset"><button type="button">Factory Reset</button></a>
</body>
</html>
)rawliteral";

static const char PROGMEM CONFIGUREGWMQTT_HTML[] = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name = "viewport" content = "width = device-width, initial-scale = 1.0, maximum-scale = 1.0, user-scalable=0">
  <title>RFM69 Gateway MQTT Configuration</title>
  <style>
    "body { background-color: #808080; font-family: Arial, Helvetica, Sans-Serif; Color: #000000; }"
  </style>
</head>
<body>
  <h3>RFM69 Gateway MQTT Configuration</h3>
  <form method='POST' action='/configGWmqtt' enctype='multipart/form-data'>
    <label>MQTT broker</label>
    <input type='text' name='mqttbroker' value="%s" size="32" maxlength="32"><br>
    <label>MQTT client name</label>
    <input type='text' name='mqttclientname' value="%s" size="32" maxlength="32"><br>
    <label>MDNS name</label>
    <input type='text' name='mdnsname' value="%s" size="32" maxlength="32"><br>
    <p><input type='submit' value='Save changes'>
  </form>
  <p><a href="/configGW"><button type="button">Cancel</button></a>
</body>
</html>
)rawliteral";

#include <WebSocketsServer.h>     //https://github.com/Links2004/arduinoWebSockets
#include <Hash.h>
ESP8266WebServer webServer(80);
WebSocketsServer webSocket = WebSocketsServer(81);

void webSocketEvent(uint8_t num, int type, uint8_t * payload, size_t length)
{
  Serial.printf("webSocketEvent(%d, %d, ...)\r\n", num, type);
  switch(type) {
    case WStype_DISCONNECTED:
      Serial.printf("[%u] Disconnected!\r\n", num);
      break;
    case WStype_CONNECTED:
      {
        IPAddress ip = webSocket.remoteIP(num);
        Serial.printf("[%u] Connected from %d.%d.%d.%d url: %s\r\n", num, ip[0], ip[1], ip[2], ip[3], payload);
        // Send the RFM69 radio configuration one time after connection
        webSocket.sendTXT(num, RadioConfig, strlen(RadioConfig));
      }
      break;
    case WStype_TEXT:
      Serial.printf("[%u] get Text: %s\r\n", num, payload);

      // send data to all connected clients
      //webSocket.broadcastTXT(payload, length);
      break;
    case WStype_BIN:
      Serial.printf("[%u] get binary length: %u\r\n", num, length);
      //hexdump(payload, length);

      // echo data back to browser
      //webSocket.sendBIN(num, payload, length);
      break;
    default:
      Serial.printf("Invalid WStype [%d]\r\n", type);
      break;
  }
}

void handleRoot()
{
  Serial.print("Free heap="); Serial.println(ESP.getFreeHeap());

  webServer.send_P(200, "text/html", INDEX_HTML);
}

void handleNotFound()
{
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += webServer.uri();
  message += "\nMethod: ";
  message += (webServer.method() == HTTP_GET)?"GET":"POST";
  message += "\nArguments: ";
  message += webServer.args();
  message += "\n";
  for (uint8_t i=0; i<webServer.args(); i++){
    message += " " + webServer.argName(i) + ": " + webServer.arg(i) + "\n";
  }
  webServer.send(404, "text/plain", message);
}

void handleconfiguregw()
{
  webServer.send_P(200, "text/html", CONFIGUREGW_HTML);
}

// Reset global config back to factory defaults
void handleconfiguregwreset()
{
  pGC->checksum++;
  EEPROM.commit();
  ESP.reset();
  delay(1000);
}

#define SELECTED_FREQ(f)  ((pGC->rfmfrequency==f)?"selected":"")

void handleconfiguregwrfm69()
{
  size_t formFinal_len = strlen_P(CONFIGUREGWRFM69_HTML) + sizeof(*pGC);
  char *formFinal = (char *)malloc(formFinal_len);
  if (formFinal == NULL) {
    Serial.println("formFinal malloc failed");
    return;
  }
  snprintf_P(formFinal, formFinal_len, CONFIGUREGWRFM69_HTML,
      pGC->networkid, pGC->nodeid, pGC->encryptkey, GC_POWER_LEVEL,
      SELECTED_FREQ(RF69_315MHZ), SELECTED_FREQ(RF69_433MHZ),
      SELECTED_FREQ(RF69_868MHZ), SELECTED_FREQ(RF69_915MHZ),
      (GC_IS_RFM69HCW)?"checked":"", (GC_IS_RFM69HCW)?"":"checked",
      pGC->rfmapname
      );
  webServer.send(200, "text/html", formFinal);
  free(formFinal);
}

void handleconfiguregwrfm69Write()
{
  bool commit_required = false;
  String argi, argNamei;

  for (uint8_t i=0; i<webServer.args(); i++) {
    Serial.print(webServer.argName(i));
    Serial.print('=');
    Serial.println(webServer.arg(i));
    argi = webServer.arg(i);
    argNamei = webServer.argName(i);
    if (argNamei == "networkid") {
      uint8_t formnetworkid = argi.toInt();
      if (formnetworkid != pGC->networkid) {
        commit_required = true;
        pGC->networkid = formnetworkid;
      }
    }
    else if (argNamei == "nodeid") {
      uint8_t formnodeid = argi.toInt();
      if (formnodeid != pGC->nodeid) {
        commit_required = true;
        pGC->networkid = formnodeid;
      }
    }
    else if (argNamei == "encryptkey") {
      const char *enckey = argi.c_str();
      if (strcmp(enckey, pGC->encryptkey) != 0) {
        commit_required = true;
        strcpy(pGC->encryptkey, enckey);
      }
    }
    else if (argNamei == "rfmapname") {
      const char *apname = argi.c_str();
      if (strcmp(apname, pGC->rfmapname) != 0) {
        commit_required = true;
        strcpy(pGC->rfmapname, apname);
      }
    }
    else if (argNamei == "powerlevel") {
      uint8_t powlev = argi.toInt();
      if (powlev != GC_POWER_LEVEL) {
        commit_required = true;
        pGC->powerlevel = (GC_IS_RFM69HCW << 7) | powlev;
      }
    }
    else if (argNamei == "rfm69hcw") {
      uint8_t hcw = argi.toInt();
      if (hcw != GC_IS_RFM69HCW) {
        commit_required = true;
        pGC->powerlevel = (hcw << 7) | GC_POWER_LEVEL;
      }
    }
    else if (argNamei == "rfmfrequency") {
      uint8_t freq = argi.toInt();
      if (freq != pGC->rfmfrequency) {
        commit_required = true;
        pGC->rfmfrequency = freq;
      }
    }
  }
  handleRoot();
  if (commit_required) {
    pGC->checksum = gc_checksum();
    EEPROM.commit();
    ESP.reset();
    delay(1000);
  }
}

void handleconfiguregwmqtt()
{
  size_t formFinal_len = strlen_P(CONFIGUREGWMQTT_HTML) + sizeof(*pGC);
  char *formFinal = (char *)malloc(formFinal_len);
  if (formFinal == NULL) {}
  snprintf_P(formFinal, formFinal_len, CONFIGUREGWMQTT_HTML,
      pGC->mqttbroker, pGC->mqttclientname, pGC->mdnsname
      );
  webServer.send(200, "text/html", formFinal);
  free(formFinal);
}

void handleconfiguregwmqttWrite()
{
  bool commit_required = false;
  String argi, argNamei;

  for (uint8_t i=0; i<webServer.args(); i++) {
    Serial.print(webServer.argName(i));
    Serial.print('=');
    Serial.println(webServer.arg(i));
    argi = webServer.arg(i);
    argNamei = webServer.argName(i);
    if (argNamei == "mqttbroker") {
      const char *broker = argi.c_str();
      if (strcmp(broker, pGC->mqttbroker) != 0) {
        commit_required = true;
        strcpy(pGC->mqttbroker, broker);
      }
    }
    else if (argNamei == "mqttclientname") {
      const char *client = argi.c_str();
      if (strcmp(client, pGC->mqttclientname) != 0) {
        commit_required = true;
        strcpy(pGC->mqttclientname, client);
      }
    }
    else if (argNamei == "mdnsname") {
      const char *mdns = argi.c_str();
      if (strcmp(mdns, pGC->mdnsname) != 0) {
        commit_required = true;
        strcpy(pGC->mdnsname, mdns);
      }
    }
  }
  handleRoot();
  if (commit_required) {
    pGC->checksum = gc_checksum();
    EEPROM.commit();
    ESP.reset();
    delay(1000);
  }
}

void websock_setup(void) {
  webServer.on("/", handleRoot);
  webServer.on("/configGW", HTTP_GET, handleconfiguregw);
  webServer.on("/configGWrfm69", HTTP_GET, handleconfiguregwrfm69);
  webServer.on("/configGWrfm69", HTTP_POST, handleconfiguregwrfm69Write);
  webServer.on("/configGWmqtt", HTTP_GET, handleconfiguregwmqtt);
  webServer.on("/configGWmqtt", HTTP_POST, handleconfiguregwmqttWrite);
  webServer.on("/configGWreset", HTTP_GET, handleconfiguregwreset);
  webServer.onNotFound(handleNotFound);
  webServer.begin();

  webSocket.begin();
  webSocket.onEvent(webSocketEvent);
}

// vvvvvvvvv ESP8266 Web OTA Updater vvvvvvvvvvv
#include "ESP8266HTTPUpdateServer.h"
ESP8266HTTPUpdateServer httpUpdater;

void ota_setup() {
  httpUpdater.setup(&webServer, "/updater", "admin", "rfm69gw");
}

// ^^^^^^^^^ ESP8266 Web OTA Updater ^^^^^^^^^^^

// ^^^^^^^^^ ESP8266 web sockets ^^^^^^^^^^^

void updateClients(uint8_t senderId, int32_t rssi, const char *message);

// vvvvvvvvv RFM69 vvvvvvvvvvv
#include <RFM69.h>                //https://www.github.com/lowpowerlab/rfm69
#include <SPI.h>

#if defined(__AVR_ATmega168__) || defined(__AVR_ATmega328P__) || defined(__AVR_ATmega88) || defined(__AVR_ATmega8__) || defined(__AVR_ATmega88__)
#define RFM69_CS      10
#define RFM69_IRQ     2
#define RFM69_IRQN    digitalPinToInterrupt(RFM69_IRQ)
#define RFM69_RST     9
#define LED           13  // onboard blinky
#elif defined(__arm__)//Use pin 10 or any pin you want
// Tested on Arduino Zero
#define RFM69_CS      10
#define RFM69_IRQ     5
#define RFM69_IRQN    digitalPinToInterrupt(RFM69_IRQ)
#define RFM69_RST     6
#define LED           13  // onboard blinky
#elif defined(ESP8266)
// ESP8266
#define RFM69_CS      15  // GPIO15/HCS/D8
#define RFM69_IRQ     4   // GPIO04/D2
#define RFM69_IRQN    digitalPinToInterrupt(RFM69_IRQ)
#define RFM69_RST     2   // GPIO02/D4
#define LED           0   // GPIO00/D3, onboard blinky for Adafruit Huzzah
#else
#define RFM69_CS      10
#define RFM69_IRQ     2
#define RFM69_IRQN    digitalPinToInterrupt(RFM69_IRQ)
#define RFM69_RST     9
#define LED           13  // onboard blinky
#endif

RFM69 radio;

void radio_setup(void) {
  int freq;
  static const char PROGMEM JSONtemplate[] =
    R"({"msgType":"config","freq":%d,"rfm69hcw":%d,"netid":%d,"power":%d})";
  char payload[128];

  radio = RFM69(RFM69_CS, RFM69_IRQ, GC_IS_RFM69HCW, RFM69_IRQN);
  // Hard Reset the RFM module
  pinMode(RFM69_RST, OUTPUT);
  digitalWrite(RFM69_RST, HIGH);
  delay(100);
  digitalWrite(RFM69_RST, LOW);
  delay(100);

  // Initialize radio
  radio.initialize(pGC->rfmfrequency, pGC->nodeid, pGC->networkid);
  if (GC_IS_RFM69HCW) {
    radio.setHighPower();    // Only for RFM69HCW & HW!
  }
  radio.setPowerLevel(GC_POWER_LEVEL); // power output ranges from 0 (5dBm) to 31 (20dBm)

  if (pGC->encryptkey[0] != '\0') radio.encrypt(pGC->encryptkey);

  pinMode(LED, OUTPUT);

  Serial.print("\nListening at ");
  switch (pGC->rfmfrequency) {
    case RF69_433MHZ:
      freq = 433;
      break;
    case RF69_868MHZ:
      freq = 868;
      break;
    case RF69_915MHZ:
      freq = 915;
      break;
    case RF69_315MHZ:
      freq = 315;
      break;
    default:
      freq = -1;
      break;
  }
  Serial.print(freq); Serial.print(' ');
  Serial.print(pGC->rfmfrequency); Serial.println(" MHz");

  size_t len = snprintf_P(RadioConfig, sizeof(RadioConfig), JSONtemplate,
      freq, GC_IS_RFM69HCW, pGC->networkid, GC_POWER_LEVEL);
  if (len >= sizeof(RadioConfig)) {
    Serial.println("\n\n*** RFM69 config truncated ***\n");
  }
}

void radio_loop(void) {
  //check if something was received (could be an interrupt from the radio)
  if (radio.receiveDone())
  {
    uint8_t senderId;
    int16_t rssi;
    uint8_t data[RF69_MAX_DATA_LEN];

    //save packet because it may be overwritten
    senderId = radio.SENDERID;
    rssi = radio.RSSI;
    memcpy(data, (void *)radio.DATA, radio.DATALEN);
    //check if sender wanted an ACK
    if (radio.ACKRequested())
    {
      radio.sendACK();
    }
    radio.receiveDone(); //put radio in RX mode
    updateClients(senderId, rssi, (const char *)data);
  } else {
    radio.receiveDone(); //put radio in RX mode
  }
}

// ^^^^^^^^^ RFM69 ^^^^^^^^^^^

// vvvvvvvvv MQTT vvvvvvvvvvv
// *** Be sure to modify PubSubClient.h ***
//
// Be sure to increase the MQTT maximum packet size by modifying
// PubSubClient.h. Add the following line to the top of PubSubClient.h.
// Failure to make this modification means no MQTT messages will be
// published.
// #define MQTT_MAX_PACKET_SIZE 256

#include <PubSubClient.h>

WiFiClient espClient;
PubSubClient mqttClient(espClient);

void callback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] ");
  for (unsigned int i = 0; i < length; i++) {
    Serial.print((char)payload[i]);
  }
  Serial.println();

  // TBD send message to RFM node
//  if (radio.sendWithRetry()) {
//  }
}

void mqtt_setup() {
  mqttClient.setServer(pGC->mqttbroker, 1883);
  mqttClient.setCallback(callback);
}

void reconnect() {
  static const char PROGMEM RFMOUT_TOPIC[] = "rfmOut/%d/#";
  char sub_topic[32];

  // Loop until we're reconnected
  while (!mqttClient.connected()) {
    Serial.print("Attempting MQTT connection...");
    // Attempt to connect
    if (mqttClient.connect(WiFi.hostname().c_str())) {
      Serial.println("connected");
      // Once connected, publish an announcement...
      mqttClient.publish("rfmIn", "Connect");
      // ... and resubscribe
      snprintf_P(sub_topic, sizeof(sub_topic), RFMOUT_TOPIC, pGC->networkid);
      mqttClient.subscribe(sub_topic);
      Serial.printf("subscribe topic [%s]\r\n", sub_topic);
    } else {
      Serial.print("failed, rc=");
      Serial.print(mqttClient.state());
      Serial.println(" try again in 2 seconds");
      // Wait 2 seconds before retrying
      delay(2000);
    }
  }
}

void mqtt_loop() {
  if (!mqttClient.connected()) {
    reconnect();
  }
  mqttClient.loop();
}

// ^^^^^^^^^ MQTT ^^^^^^^^^^^

struct _nodestats {
  unsigned long recvMessageCount;
  unsigned long recvMessageMissing;
  unsigned long recvMessageDuplicate;
  unsigned long recvMessageSequence;
};

typedef struct _nodestats nodestats_t;
nodestats_t *nodestats[256];  // index by node ID

struct _nodestats *get_nodestats(uint8_t nodeID)
{
  if (nodestats[nodeID] == NULL) {
    nodestats[nodeID] = (nodestats_t *)malloc(sizeof(nodestats_t));
    if (nodestats[nodeID] == NULL) {
      Serial.println("\n\n*** nodestats malloc() failed ***\n");
      return NULL;
    }
    memset(nodestats[nodeID], 0, sizeof(nodestats_t));
  }
  return nodestats[nodeID];
}

void updateClients(uint8_t senderId, int32_t rssi, const char *message)
{
  nodestats_t *ns;
  ns = get_nodestats(senderId);
  if (ns == NULL) {
    Serial.println("\n\n*** updatedClients failed ***\n");
    return;
  }
  unsigned long sequenceChange, newMessageSequence;
  static const char PROGMEM JSONtemplate[] =
    R"({"msgType":"status","rxMsgCnt":%lu,"rxMsgMiss":%lu,"rxMsgDup":%lu,"senderId":%d,"rssi":%d,"message":"%s"})";
  char payload[192], topic[32];

  //Serial.printf("\r\nnode %d msg %s\r\n", senderId, message);
  newMessageSequence = strtoul(message, NULL, 10);
  ns->recvMessageCount++;
  //Serial.printf("nms %lu rmc %lu rms %lu\r\n",
  //    newMessageSequence, ns->recvMessageCount, ns->recvMessageSequence);
  if (ns->recvMessageCount != 1) {
    // newMessageSequence == 0 means the sender just start up.
    // Or the counter wrapped. But the counter is uint32_t so
    // that will take a very long time.
    if (newMessageSequence != 0) {
      if (newMessageSequence == ns->recvMessageSequence) {
        ns->recvMessageDuplicate++;
      }
      else {
        if (newMessageSequence > ns->recvMessageSequence) {
          sequenceChange = newMessageSequence - ns->recvMessageSequence;
        }
        else {
          sequenceChange = 0xFFFFFFFFUL - (ns->recvMessageSequence - newMessageSequence);
        }
        if (sequenceChange > 1) {
          ns->recvMessageMissing += sequenceChange - 1;
        }
      }
    }
  }
  ns->recvMessageSequence = newMessageSequence;
  //Serial.printf("nms %lu rmc %lu rms %lu\r\n",
  //    newMessageSequence, ns->recvMessageCount, ns->recvMessageSequence);

  // Send using JSON format (http://www.json.org/)
  // The JSON will look like this:
  // {
  //   "rxMsgCnt": 123,
  //   "rxMsgMiss": 0,
  //   "rxMsgDup": 0,
  //   "senderId": 12,
  //   "rssi": -30,
  //   "message": "Hello World #1234"
  // }
  size_t len = snprintf_P(payload, sizeof(payload), JSONtemplate,
      ns->recvMessageCount, ns->recvMessageMissing,
      ns->recvMessageDuplicate, senderId, rssi, message);
  if (len >= sizeof(payload)) {
    Serial.println("\n\n*** RFM69 packet truncated ***\n");
  }
  // send received message to all connected web clients
  webSocket.broadcastTXT(payload, strlen(payload));

  // mqtt publish the same message
  len = snprintf(topic, sizeof(topic), "rfmIn/%d/%d", NETWORKID, senderId);
  if (len >= sizeof(topic)) {
    Serial.println("\n\n*** MQTT topic truncated ***\n");
  }
  if ((strlen(payload)+1+strlen(topic)+1) > MQTT_MAX_PACKET_SIZE) {
    Serial.println("\n\n*** MQTT message too long! ***\n");
  }
  //Serial.printf("topic [%s] message [%s]\r\n", topic, payload);
  if (!mqttClient.publish(topic, payload)) {
    Serial.println("\n\n*** mqtt publish failed ***\n");
  }
}


void setup() {
  Serial.begin(SERIAL_BAUD);
  Serial.println("\nRFM69 WiFi Gateway");

  Serial.println(ESP.getResetReason());

  // Adafruit Huzzah has an LED on GPIO0 with negative logic.
  // Turn if off.
  pinMode(0, OUTPUT);
  digitalWrite(0, HIGH);

  eeprom_setup();
  wifi_setup();
  mdns_setup();
  mqtt_setup();
  ota_setup();
  websock_setup();
  radio_setup();
}

void loop() {
  radio_loop();
  mqtt_loop();
  webSocket.loop();
  webServer.handleClient();
}

