#include <Wire.h>
#include <MCP342x.h>
#include <EEPROM.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <ESP8266WiFiMulti.h>
#include <ESP8266WebServer.h>

/**
 * One phase energy monitor based on ESP8266, MCP3421.
 * 
 * @author Pavel Petrzela
*/

// Magic, abych poznal, jestli jsem zapsal do EEPROM ja
#define MAGIC 0xA6

// Timeout konfigurace
#define CONFIG_TIMEOUT 1000 * 60 * 5

// Nazev WIFI AP
#define WIFI_AP_SSID "EnergyMonitorAP"

// Timeout A/D konverze
#define AD_TIMEOUT 2000

// Port WEB serveru konfigurace
#define WEB_SERVER_PORT 80

// Maximalni pocet vzorku do odeslani
#define MAX_SAMPLE_FREQUENCY 256

// Maximalni pocet radku v logu
#define MAX_LOG_LINES 50

// Maximalni index fieldu na ThingSpeak
#define TS_MAX_FIELD 8

// adresa MCP3421 A/D prevodniku
const uint8_t address = 0x68;

// Komunikacni objekt
MCP342x adc = MCP342x (address);

// Wifi
ESP8266WiFiMulti wifiMulti;

// Struktura do EEPROM
struct EmConfiguration {
  int magic;
  int sampleFrequency;
  char apName [24];
  char password [48];
  long voltage;
  long offset;
  long divider;
  char tsWriteKey [20];
  int tsFieldId;
};

// Nastaveni
EmConfiguration config;

// Server pro konfiguraci
ESP8266WebServer *server;

// Priznak, zdali byla spustena konfigurace
bool configExecuted = false;

// Posledni interakce s konfiguraci
long lastInteractionTime;

// Seznam radku logu
String logLines [MAX_LOG_LINES];
int logLinesIndex = 0;

// Posledni status
String lastStatus;

/**
 * Metoda je provedena pri inicializaci.
*/
void setup(void) {
  Serial.begin (115200);
  // Vypneme WiFi, aby se to nazacatek o nic nepokouselo
  WiFi.mode (WIFI_OFF);

  delay (10000);

  int i;
  for (i = 0; i < MAX_LOG_LINES; i++) {
    logLines [i] = "";
  }

  emLogLine ("setup()");

  // Komunikace po i2c sbernici na pinech 0 (sda) a 2 (scl)
  Wire.begin (0, 2);

  // Zkusime najit MCP3421
  do {
    emLogLine ("I2C reset...");
    // Reset sbernice
    MCP342x::generalCallReset();
    delay (10);

    emLogLine ("Searching MCP3421...");
  
    // Zjistime, zdali je MCP3421 na sbernici
    Wire.requestFrom (address, (uint8_t)1);
    if (!Wire.available()) {
      emLog ("No MCP3421 found at address ");
      emLogLine (String(address));
      delay (5000);
    } else {
      break;
    }
  } while (1);

  emLogLine ("MCP3421 found... reading configuration from EEPROM...");

  // nacteni konfigurace; pokud konfigurace jeste nebyla
  // vytvorena, tak se vymaze a inicializuje defaultnimi
  // hodnotami
  EEPROM.begin (sizeof (EmConfiguration));
  EEPROM.get (0, config);

  if (config.magic != MAGIC) {
    memset (&config, 0, sizeof (EmConfiguration));
    config.magic = MAGIC;
    config.sampleFrequency = 50;
    config.voltage = 230;
    config.offset = 523L;
    config.divider = 3805L;
    config.tsFieldId = 1;
    EEPROM.put (0, config);
  }
  
  EEPROM.end();

  emLog ("Configuration readed .. starting AP ");
  emLogLine (WIFI_AP_SSID);

  // Spustime konfiguracni AccessPoint pod nazvem EnergyMonitorAP
  WiFi.softAP (WIFI_AP_SSID);

  // ... a spustime HTTP server pro nastaveni; konfiguracni server
  // pobezi 5 minut od spusteni, pak 
  server = new ESP8266WebServer (WiFi.softAPIP(), WEB_SERVER_PORT);
  server->on ("/", wsHandleRoot);
  server->on ("/update", wsHandleUpdate);
  server->on ("/run", wsHandleRun);
  server->on ("/log", wsDisplayLog);
  server->on ("/getAdc", wsGetAdc);
  server->begin();

  emLog ("Started WebServer at ");
  emLog (ipToString (WiFi.softAPIP()));
  emLogLine (" port 80...");

  lastInteractionTime = millis();
  lastStatus = "";
}

/**
 * Prevede IP adresu do retezce.
*/
String ipToString (IPAddress myIP) {
  return String(myIP[0])+"."+String(myIP[1])+"."+String(myIP[2])+"."+String(myIP[3]);
}

/**
 * Prida radek do logu.
 * 
 * @param line radek.
*/
void emLogLine (String line) {
  if (logLinesIndex >= MAX_LOG_LINES - 1) {
    logLines [logLinesIndex] += line;
    
    int i;
    for (i = 1; i < MAX_LOG_LINES; i++) {
      logLines [i - 1] = logLines [i];
    }

    logLines [MAX_LOG_LINES - 1] = "";
  } else {
    logLines [logLinesIndex++] += line;
  }

  Serial.println (line);
}

/**
 * Pripoji retezec za posledni radek logu.
 * 
 * @param str retezec.
*/
void emLog (String str) {
  if (logLinesIndex >= MAX_LOG_LINES - 1) {
    logLines [MAX_LOG_LINES - 1] += str;
  } else {
    logLines [logLinesIndex] += str;
  }

  Serial.print (str);
}

/**
 * Aktualizuje retezcovou promennou v konfiguraci na zaklade
 * predaneho retezce.
 *
 * @param c promenna.
 * @param len maximalni delka.
 * @param val arduino string pro prevedeni.
*/
void updateConfigKey (char *c, int len, String val) {
  memset (c, 0, len);
  sprintf (c, "%s", val.c_str());
}

/**
 * Zapise koren http serveru; konfiguracni obrazovku.
*/
void wsHandleRoot() {
  String resp = "<html><head><title>Energy monitor configuration</title>";
  resp += "<meta name=\"viewport\" content=\"initial-scale=1.0, width = device-width, user-scalable = no\">";
  resp += "<script>";
  resp += "function ebi(s) {";
  resp += "return document.getElementById (s);";
  resp += "}\n";
  resp += "function updateAdc() {";
  resp += "var xhttp = new XMLHttpRequest();";
  resp += "xhttp.onreadystatechange = function() {";
  resp += "  if ((this.readyState == 4) && (this.status == 200)) {";
  resp += "    var adcVal = parseInt (this.responseText);";
  resp += "    var form = ebi('form');";
  resp += "    var voltage = parseInt (form.voltage.value);";
  resp += "    var offset = parseInt (form.offset.value);";
  resp += "    var divider = parseInt (form.divider.value);";
  resp += "    var current = ((adcVal - offset) / divider);";
  resp += "    if (current < 0) current = 0;";
  resp += "    var output = ebi('info');";
  resp += "    var s = 'ADC: <b>' + adcVal + '</b><br>POWER: <b>' + Number (voltage * current).toFixed (5) + 'W</b><br>MAX_POWER: <b>' + Number(voltage * ((131071 - offset) / divider)).toFixed (2) + 'W</b>';";
  resp += "    output.innerHTML = s;";
  resp += "    window.setTimeout(updateAdc, 1000);";
  resp += "  }";
  resp += "};";
  resp += "xhttp.open(\"GET\", \"/getAdc\", true);";
  resp += "xhttp.send();";
  resp += "}\n";
  resp += "function submitMode (mode) {";
  resp += "var f = ebi('form');";
  resp += "f.mode.value = mode;";
  resp += "f.submit();";
  resp += "}\n";
  resp += "</script>";
  resp += "</head><body>";
  resp += "<h1>Energy monitor configuration</h1>";
  resp += "<form method=\"post\" action=\"/update\" id=\"form\">";
  resp += "<input type=\"hidden\" name=\"mode\" value=\"save\">";
  resp += "<table border=\"0\" cellspacing=\"0\" cellpadding=\"5\">";
  resp += "<tr><td>AP SSID:</td><td><input type=\"text\" name=\"apName\" value=\"" + String(config.apName) + "\" maxlength=\"24\"></td><td></td></tr>";
  resp += "<tr><td>AP Password:</td><td><input type=\"password\" name=\"password\" value=\"" + String(config.password) + "\" maxlength=\"48\"></td><td></td></tr>";
  resp += "<tr><td>Voltage:</td><td><input type=\"text\" name=\"voltage\" value=\"" + String(config.voltage) + "\"></td><td></td></tr>";
  resp += "<tr><td>Offset:</td><td><input type=\"text\" name=\"offset\" value=\"" + String(config.offset) + "\"></td><td><input type=\"button\" onclick=\"submitMode('calibrate');\" value=\"Calibrate\"/></td></tr>";
  resp += "<tr><td>Divider:</td><td><input type=\"text\" name=\"divider\" value=\"" + String(config.divider) + "\"></td><td></td></tr>";
  resp += "<tr><td>ThingSpeak key:</td><td><input type=\"text\" name=\"tsWriteKey\" value=\"" + String(config.tsWriteKey) + "\" maxlength=\"16\"></td><td></td></tr>";
  resp += "<tr><td>ThingSpeak field:</td><td><input type=\"text\" name=\"tsFieldId\" value=\"" + String(config.tsFieldId) + "\" maxlength=\"1\"></td><td></td></tr>";
  resp += "<tr><td>Sample frequency:</td><td><input type=\"text\" name=\"sampleFrequency\" value=\"" + String(config.sampleFrequency) + "\"></td><td></td></tr>";
  resp += "<tr><td colspan=\"3\" align=\"center\"><input type=\"submit\"></td></tr>";
  resp += "</table></form>";
  resp += "<code id=\"info\"></code>";
  resp += "<p><a href=\"/log\">Last " + String(MAX_LOG_LINES) + " log lines</a></p>";
  if (!configExecuted) {
    resp += "<p><a href=\"/run\">Start monitoring</a></p>";
  }
  resp += "<script>window.setTimeout(updateAdc, 1000);</script></body></html>";

  server->send(200, "text/html", resp);

  lastInteractionTime = millis();
}

/**
 * Aktualizace nastaveni.
*/
void wsHandleUpdate() {
  emLogLine ("handleUpdate()");
  yield();
  String mode = server->arg("mode");
  String apName = server->arg ("apName");
  String password = server->arg ("password");
  long voltage = atol (server->arg ("voltage").c_str());
  long offset = atol (server->arg ("offset").c_str());
  long divider = atol (server->arg ("divider").c_str());
  String tsWriteKey = server->arg ("tsWriteKey");
  int tsFieldId = atoi (server->arg ("tsFieldId").c_str());
  int sampleFrequency = atoi (server->arg ("sampleFrequency").c_str());

  emLog ("mode="); emLogLine (mode);
  emLog ("apName="); emLogLine (apName);
  emLog ("password="); emLogLine (password);
  emLog ("voltage="); emLogLine (String(voltage));
  emLog ("offset="); emLogLine (String(offset));
  emLog ("divider="); emLogLine (String(divider));
  emLog ("tsWriteKey="); emLogLine (tsWriteKey);
  emLog ("tsFieldId="); emLogLine (String(tsFieldId));
  emLog ("sampleFrequency="); emLogLine (String(sampleFrequency));

  // pojistka proti nacteni jinak nez postem
  if (apName.length() > 1) {
    if (mode == "save") {
      // aktualizujeme konfiguraci
      updateConfigKey (config.apName, 24, apName);
      updateConfigKey (config.password, 48, password);
      config.voltage = voltage;
      config.offset = offset;
      config.divider = divider;
      updateConfigKey (config.tsWriteKey, 16, tsWriteKey);
      config.tsFieldId = constrain (tsFieldId, 1, TS_MAX_FIELD);
      config.sampleFrequency = constrain (sampleFrequency, 1, MAX_SAMPLE_FREQUENCY);
  
      // zapiseme do EEPROM
      EEPROM.begin (sizeof (EmConfiguration));
      EEPROM.put (0, config);
      EEPROM.end();
  
      String resp = "<script>window.alert ('Configuration updated...'); window.location.replace ('/');</script>";
      server->send (200, "text/html", resp);
    } else if (mode == "calibrate") {
      wsCalibrateOffset();
    }
  } else {
    server->send (200, "text/html", "");
  }

  lastInteractionTime = millis();
}

/**
 * Spusteni monitorovani. 
*/
void wsHandleRun() {
  configExecuted = true;

  String resp = "<script>window.alert ('Monitoring started...'); window.location.replace ('/');</script>";
  server->send (200, "text/html", resp);

  lastInteractionTime = millis();
}

/**
 * Kalibrace offsetu.
*/
void wsCalibrateOffset() {
  String result = "";
  int sample;
  long maxVal = 0L;

  for (sample = 1; sample < 60; sample++) {
    long value = getAdc();

    if (value > maxVal) {
      maxVal = value;
    }

    delay (500);
    yield();
  }

  config.offset = maxVal;

  String resp = "<script>window.location.replace('/');</script>";
  server->send (200, "text/html", resp);

  lastInteractionTime = millis();
}

/**
 * Zobrazeni logu.
*/
void wsDisplayLog() {
  String resp = "<html><head><title>Last " + String (MAX_LOG_LINES) + "</title>";
  resp += "</head><body><h1>Last " + String (MAX_LOG_LINES) + " log lines</h1><pre>";
  int i;
  for (i = 0; i <= logLinesIndex; i++) {
    resp += logLines [i] + "<br>";
  }
  resp += "</pre></body></html>";

  server->send (200, "text/html", resp);
  
  lastInteractionTime = millis();
}

/**
 * Aktualni hodnota z MCP3421.
*/
void wsGetAdc() {
  String resp (getAdc());
  server->send (200, "text/plain", resp);
  
  lastInteractionTime = millis();  
}

/**
 * Metoda vraci prevedenou hodnotu z MCP3421.
 * 
 * @return long hodnota.
*/
long getAdc() {
  long value = 0;
  MCP342x::Config status;
  
  uint8_t err = adc.convertAndRead (MCP342x::channel1, MCP342x::continous, MCP342x::resolution18, MCP342x::gain2, AD_TIMEOUT, value, status);
    
  if (err) {
    emLog ("Convert error: ");
    emLog (String(err));
    emLogLine ("!");
    return -1L;
  } else {
    return value;
  }
}

/**
 * Zakodovani parametru URL.
 * 
 * @param str retezec.
 * @return String zakodovany.
*/
String urlencode (String str) {
  String encodedString="";
  char c;
  char code0;
  char code1;
  for (int i =0; i < str.length(); i++){
    c=str.charAt(i);
    if (c == ' '){
      encodedString+= '+';
    } else if (isalnum(c)){
      encodedString+=c;
    } else{
      code1=(c & 0xf)+'0';
      if ((c & 0xf) >9){
        code1=(c & 0xf) - 10 + 'A';
      }
      c=(c>>4)&0xf;
      code0=c+'0';
      if (c > 9){
        code0=c - 10 + 'A';
      }
      encodedString+='%';
      encodedString+=code0;
      encodedString+=code1;
    }
    yield();
  }
  return encodedString;
}

/**
 * Odpoji a sjedna spojeni na WiFi.
*/
void reconnectWifi() {
  // zavreme HTTP server a spustime WiFi pouze v modu klienta tj. zrusime access point
  server->close();
  
  delay (1000);
  WiFi.disconnect();
  delay (2000);
  WiFi.mode (WIFI_STA);
  delay (2000);
  
  // pridame nastavenou WiFinu; jinak to pojede na jakekoliv verejne, ale vybere to
  // tu nejsilnejsi
  emLog ("Connecting to WiFi ");
  emLog (config.apName);
  
  wifiMulti.addAP (config.apName, config.password);
  delay (5000);
  while (wifiMulti.run() != WL_CONNECTED) {
    yield();
    delay(1000);
    emLog (".");
  }
  emLogLine("");

  emLogLine ("WiFi connected...");

  server = new ESP8266WebServer (WiFi.localIP(), WEB_SERVER_PORT);
  server->on ("/", wsHandleRoot);
  server->on ("/update", wsHandleUpdate);
  server->on ("/run", wsHandleRun);
  server->on ("/log", wsDisplayLog);
  server->on ("/getAdc", wsGetAdc);
  server->begin();
  emLog ("Started WebServer at ");
  emLog (ipToString (WiFi.localIP()));
  emLogLine (" port 80...");
}

/**
 * Smycka monitorovani spotreby a odesilani na ThingSpeak.
*/
void thingSpeakLoop() {
  emLog ("Config timeout expired ... closing ");
  emLog (WIFI_AP_SSID);
  emLogLine (" and starting monitoring...");

  reconnectWifi();

  // buffer pro prumerovani hodnot
  long buffer [MAX_SAMPLE_FREQUENCY];
  int bufferPos = 0;
  int errorCount = 0;

  while (true) {
    // Obsluha HTTP
    server->handleClient();
    
    // nacteme hodnotu z MCP3421
    long value = getAdc();
    if (value >= 0L) {
      // ulozime hodnotu na dalsi pozici v bufferu
      buffer [bufferPos] = value;
      bufferPos++;

      if (bufferPos >= config.sampleFrequency) {
        bufferPos = 0;

        // pokud je buffer zaplnen, vypocitame prumernou hodnotu
        unsigned long average = 0L;
        for (int i = 0; i < config.sampleFrequency; i++) {
          long val = buffer [i] - config.offset;
          if (val >= 0L) {
            average += val;
          }
        }
        average /= config.sampleFrequency;

        // vypocitame proud a vykon
        double current = double(average) / double(config.divider);
        double power = current * double(config.voltage);

        char number [20];
        dtostrf (current, 5, 6, number);
 
        emLog (number);
        emLog ("A, ");
  
        dtostrf (power, 5, 3, number);
  
        emLog (number);
        emLog ("W -> ");

        if (strlen (config.tsWriteKey) < 1) {
          emLogLine ("ThingSpeak write key is not set!");
        } else if (wifiMulti.run() == WL_CONNECTED) {
          // pokud mame spojeni na WiFi, aktualizujeme ThingSpeak
          HTTPClient http;

          String stat = "OK; WebServer listening at " + ipToString (WiFi.localIP()) + ":" + String(WEB_SERVER_PORT);
          String statStr;
          if (stat == lastStatus) {
            statStr = "";
          } else {
            statStr = "&status=" + urlencode (stat); 
          }

          String sNumber (number);
          sNumber.trim();
  
          char url [512];
          sprintf (url, "http://api.thingspeak.com/update.json?field%d=%s%s&api_key=%s", config.tsFieldId, sNumber.c_str(), statStr.c_str(), config.tsWriteKey);
  
          http.begin (url);
  
          int httpCode = http.GET();
          if (httpCode == HTTP_CODE_OK) {
            emLog ("-> ");
            emLog (url);
            emLogLine (" -> [HTTP] UPDATE OK");

            lastStatus = stat;
            errorCount = 0;
          } else {
            emLog ("[HTTP] GET... failed, error(");
            emLog (String(httpCode));
            emLog ("): url=");
            emLog (url);
            emLog (" -> ");
            emLogLine (http.errorToString(httpCode));
            errorCount++;
          }
  
          http.end();
        } else {
          emLogLine ("WiFi NOT CONNECTED");
          errorCount++;
        }
      }
    }

    if (errorCount > 30) {
      emLogLine ("ErrorCount > 30; reconnecting WiFi...");
      errorCount = 0;
      reconnectWifi();
    }
  
    // dame prostor procesum na pozadi
    yield();
  }
}

/**
 * Periodicky vykonavana metoda. 
*/
void loop (void) {
  if (configExecuted) {
    thingSpeakLoop();
  } else {
    boolean configValid = (strlen (config.tsWriteKey) > 0) && (strlen (config.apName) > 0);

    if (!configValid || (millis() - lastInteractionTime < CONFIG_TIMEOUT)) {
      server->handleClient();
    } else {
      configExecuted = true; 
    }
  }
}

