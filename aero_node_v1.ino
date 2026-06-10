/*
 * Project: AeroNode by Shrut and Siddhi
 * Description: Smart Air Quality Monitor using MQ-135
 * Hardware: ESP32, DHT11/22, MQ-135, I2C 16x2 LCD
 */

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <LittleFS.h>
#include <DHT.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ---------------- CONFIGURATION ----------------
#define SSID_AP "AeroNode"
#define PASS_AP "12345678"

#define LOG_FILE "/aeronode_logs.csv"
#define META_FILE "/meta.bin"
#define CALIB_FILE "/calib.bin"

#define DHTPIN 4
#define DHTTYPE DHT11
#define MQ135_PIN 34
#define LCD_ADDR 0x27

#define MAX_LOG_LINES 5000
#define DRIFT_SMOOTHING 0.0001f

const byte DNS_PORT = 53;
IPAddress apIP(192, 168, 4, 1);
DNSServer dnsServer;

// ---------------- OBJECTS ----------------
DHT dht(DHTPIN, DHTTYPE);
LiquidCrystal_I2C lcd(LCD_ADDR, 16, 2);
WebServer server(80);

// ---------------- GLOBAL DATA ----------------
struct SystemState {
  float temp = 25.0;
  float hum = 50.0;
  float aqi = 20.0;
  float ci = 1.0;
  char aqiS[12] = "Checking";
  char bact[16] = "Safe";
  char risk[8] = "Low";
  float storage = 0;
  unsigned long logCounter = 0;
  unsigned long currentLines = 0;
  float calibratedBaseline = 400.0f;
} aero;

char activeLcdMsg[65] = "AeroNode by Shrut and Siddhi";
unsigned long lcdMsgEnd = 0;

unsigned long topScrollTimer = 0;
unsigned long bottomScrollTimer = 0;
unsigned long logTimer = 0;
unsigned long sensorTimer = 0;

int topScrollPos = 0;
int bottomScrollPos = 0;

int logSyncCounter = 0;
bool isCalibrating = false;
unsigned long calibLastSample = 0;
int calibSamplesCount = 0;
long calibTotal = 0;

// ---------------- PERSISTENCE HELPERS ----------------

void saveMetadata() {
  File f = LittleFS.open(META_FILE, "w");
  if (f) {
    f.write((uint8_t*)&aero.logCounter, sizeof(aero.logCounter));
    f.write((uint8_t*)&aero.currentLines, sizeof(aero.currentLines));
    f.close();
  }
}

void loadMetadata() {
  if (LittleFS.exists(META_FILE)) {
    File f = LittleFS.open(META_FILE, "r");
    if (f) {
      f.read((uint8_t*)&aero.logCounter, sizeof(aero.logCounter));
      f.read((uint8_t*)&aero.currentLines, sizeof(aero.currentLines));
      f.close();
    }
  } else {
    File f = LittleFS.open(LOG_FILE, "r");
    if (f) {
      aero.currentLines = 0;
      while (f.available()) {
        f.readStringUntil('\n');
        aero.currentLines++;
      }
      aero.logCounter = aero.currentLines;
      f.close();
      saveMetadata();
    }
  }
}

void saveCalibration() {
  File f = LittleFS.open(CALIB_FILE, "w");
  if (f) {
    f.write((uint8_t*)&aero.calibratedBaseline, sizeof(aero.calibratedBaseline));
    f.close();
  }
}

void loadCalibration() {
  if (LittleFS.exists(CALIB_FILE)) {
    File f = LittleFS.open(CALIB_FILE, "r");
    if (f) {
      f.read((uint8_t*)&aero.calibratedBaseline, sizeof(aero.calibratedBaseline));
      f.close();
    }
  }
}

// ---------------- DASHBOARD ----------------

const char INDEX_HTML[] PROGMEM = R"AERONODE_HTML(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>AeroNode Dashboard</title>
    <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
    <style>
        :root {
            --bg: #0d0d12;
            --card: #1c1c24;
            --accent: #10a37f;
            --text: #ececf1;
            --border: #343441;
            --warn: #f4c025;
        }

        body {
            font-family: Arial, sans-serif;
            background: var(--bg);
            color: var(--text);
            margin: 0;
            padding: 15px;
        }

        .container {
            max-width: 900px;
            margin: 0 auto;
        }

        header {
            border-bottom: 1px solid var(--border);
            padding-bottom: 10px;
            margin-bottom: 20px;
            display: flex;
            justify-content: space-between;
            align-items: center;
            gap: 12px;
        }

        h1 {
            font-size: 1.2rem;
            color: var(--accent);
            margin: 0;
        }

        .grid {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
            gap: 15px;
        }

        .card {
            background: var(--card);
            border-radius: 12px;
            padding: 20px;
            border: 1px solid var(--border);
        }

        .label {
            font-size: 0.7rem;
            color: #8e8ea0;
            text-transform: uppercase;
            font-weight: 600;
        }

        .value {
            font-size: 1.8rem;
            font-weight: 700;
            margin: 8px 0;
        }

        .status {
            font-size: 0.8rem;
            color: var(--accent);
        }

        .graph-container {
            height: 100px;
            margin-top: 10px;
        }

        .panel {
            background: var(--card);
            border-radius: 12px;
            padding: 20px;
            margin-top: 20px;
            border: 1px solid var(--border);
        }

        .input-row {
            display: flex;
            gap: 10px;
            margin-top: 10px;
        }

        input {
            flex: 1;
            background: #2a2a35;
            border: 1px solid var(--border);
            border-radius: 8px;
            color: white;
            padding: 10px;
            outline: none;
            min-width: 0;
        }

        button {
            background: var(--accent);
            color: white;
            border: none;
            padding: 10px 15px;
            border-radius: 8px;
            cursor: pointer;
            font-weight: 600;
        }

        .storage-bar {
            background: #2d2d39;
            height: 8px;
            border-radius: 4px;
            margin: 10px 0;
            overflow: hidden;
        }

        #bar-fill {
            height: 100%;
            background: var(--accent);
            width: 0%;
            transition: 0.5s;
        }

        .nav-links {
            display: flex;
            gap: 10px;
            margin-top: 10px;
            flex-wrap: wrap;
        }

        .nav-links button {
            font-size: 0.8rem;
            flex-grow: 1;
        }

        .calib-pulse {
            animation: pulse 1.5s infinite;
            color: var(--warn) !important;
            font-weight: bold;
        }

        @keyframes pulse {
            0% { opacity: 1; }
            50% { opacity: 0.4; }
            100% { opacity: 1; }
        }

        @media (max-width: 520px) {
            header {
                align-items: flex-start;
                flex-direction: column;
            }

            .input-row {
                flex-direction: column;
            }

            input[type="number"] {
                width: auto !important;
            }
        }
    </style>
</head>
<body>
    <div class="container">
        <header>
            <h1>AeroNode by Shrut and Siddhi</h1>
            <div id="sys-status" class="status">System Online</div>
        </header>

        <div class="grid">
            <div class="card">
                <div class="label">Estimated AQI</div>
                <div class="value" id="aqi">--</div>
                <div class="status" id="aqiS">--</div>
                <div class="graph-container"><canvas id="chartAQI"></canvas></div>
            </div>

            <div class="card">
                <div class="label">Temperature</div>
                <div class="value" id="temp">-- C</div>
                <div class="status" id="humVal">--% Humidity</div>
                <div class="graph-container"><canvas id="chartTemp"></canvas></div>
            </div>

            <div class="card">
                <div class="label">Humidity</div>
                <div class="value" id="humDisp">--%</div>
                <div class="status">Live RH%</div>
                <div class="graph-container"><canvas id="chartHum"></canvas></div>
            </div>

            <div class="card">
                <div class="label">Air Condition</div>
                <div class="value" id="bact" style="font-size:1.1rem">--</div>
                <div class="status" id="risk">Risk: --</div>
                <div class="graph-container"><canvas id="chartCI"></canvas></div>
            </div>
        </div>

        <div class="panel">
            <div class="label">LCD Display Override</div>
            <div class="input-row">
                <input type="text" id="msg" placeholder="Type message..." maxlength="64">
                <input type="number" id="dur" placeholder="Secs" style="width:70px;" value="15">
                <button onclick="sendMsg()">Update LCD</button>
            </div>
        </div>

        <div class="panel">
            <div class="label">System & Storage</div>
            <div id="storage-txt" style="font-size:0.8rem; margin-top:5px;">Used: 0%</div>
            <div class="storage-bar"><div id="bar-fill"></div></div>
            <div id="warn" style="color:var(--warn); font-size:0.75rem; display:none;">Storage rotation active.</div>

            <div class="nav-links">
                <button onclick="location.href='/download'">Download CSV</button>
                <button onclick="calibrate()" id="btnCalib">Calibrate Base</button>
            </div>
        </div>
    </div>

    <script>
        var charts = {};

        function dummyChart() {
            return {
                data: {
                    labels: [],
                    datasets: [{ data: [] }]
                },
                update: function() {}
            };
        }

        function initChart(id, label, color, min, max) {
            if (typeof Chart === "undefined") {
                return dummyChart();
            }

            return new Chart(document.getElementById(id), {
                type: 'line',
                data: {
                    labels: [],
                    datasets: [{
                        label: label,
                        data: [],
                        borderColor: color,
                        tension: 0.4,
                        pointRadius: 0,
                        fill: false,
                        borderWidth: 2
                    }]
                },
                options: {
                    maintainAspectRatio: false,
                    animation: false,
                    plugins: { legend: { display: false } },
                    scales: {
                        x: { display: false },
                        y: {
                            display: false,
                            suggestedMin: min,
                            suggestedMax: max
                        }
                    }
                }
            });
        }

        charts.aqi = initChart('chartAQI', 'AQI', '#10a37f', 0, 300);
        charts.temp = initChart('chartTemp', 'Temp', '#3b82f6', 10, 45);
        charts.hum = initChart('chartHum', 'Humidity', '#0ea5e9', 0, 100);
        charts.ci = initChart('chartCI', 'CI', '#f4c025', 0, 5);

        function setText(id, text) {
            var el = document.getElementById(id);
            if (el) el.innerText = text;
        }

        function update() {
            fetch('/data?nocache=' + Date.now(), { cache: 'no-store' })
                .then(function(r) {
                    return r.json();
                })
                .then(function(d) {
                    setText('aqi', Math.round(d.aqi));
                    setText('aqiS', d.aqiS);
                    setText('temp', d.temp.toFixed(1) + " C");
                    setText('humVal', d.hum.toFixed(0) + "% Humidity");
                    setText('humDisp', d.hum.toFixed(0) + "%");
                    setText('bact', d.bact);
                    setText('risk', "Risk: " + d.risk);
                    setText('storage-txt', "Used: " + d.storage.toFixed(1) + "% (" + d.records + " logs)");

                    document.getElementById('bar-fill').style.width = d.storage + "%";
                    document.getElementById('warn').style.display = d.storage > 90 ? "block" : "none";

                    var sysStatus = document.getElementById('sys-status');

                    if (d.cal) {
                        sysStatus.innerText = "CALIBRATING...";
                        sysStatus.className = "status calib-pulse";
                    } else {
                        sysStatus.innerText = "System Online";
                        sysStatus.className = "status";
                    }

                    var mapping = [
                        ['aqi', d.aqi],
                        ['temp', d.temp],
                        ['hum', d.hum],
                        ['ci', d.ci]
                    ];

                    mapping.forEach(function(pair) {
                        var c = charts[pair[0]];
                        c.data.labels.push("");
                        c.data.datasets[0].data.push(pair[1]);

                        if (c.data.labels.length > 30) {
                            c.data.labels.shift();
                            c.data.datasets[0].data.shift();
                        }

                        c.update('none');
                    });
                })
                .catch(function() {
                    setText('sys-status', 'Waiting for data...');
                });
        }

        function sendMsg() {
            var m = document.getElementById('msg').value;
            var d = document.getElementById('dur').value;

            if (!m) return;

            fetch('/message?msg=' + encodeURIComponent(m) + '&dur=' + d, {
                method: 'POST'
            });

            document.getElementById('msg').value = "";
        }

        function calibrate() {
            if (!confirm("Start baseline calibration? Clean air is required. 5 second background process.")) return;

            fetch('/calibrate', {
                method: 'POST'
            });
        }

        setInterval(update, 2000);
        update();
    </script>
</body>
</html>
)AERONODE_HTML";

// ---------------- SENSOR & LOGIC ----------------

void updateIntelligence() {
  if (isnan(aero.temp) || isnan(aero.hum)) {
    aero.temp = 25.0;
    aero.hum = 50.0;
  }

  if (aero.aqi < 50) {
    strcpy(aero.aqiS, "Good");
  } else if (aero.aqi < 100) {
    strcpy(aero.aqiS, "Moderate");
  } else {
    strcpy(aero.aqiS, "Poor");
  }

  if (aero.temp > 29 && aero.hum > 65) {
    strcpy(aero.bact, "Bact. Growth");
    strcpy(aero.risk, "High");
  } else if (aero.aqi > 150) {
    strcpy(aero.bact, "Gas/Dust Alert");
    strcpy(aero.risk, "Med");
  } else {
    strcpy(aero.bact, "Safe/Clean");
    strcpy(aero.risk, "Low");
  }
}

void readSensors() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();

  if (!isnan(t)) aero.temp = t;
  if (!isnan(h)) aero.hum = h;

  int raw = analogRead(MQ135_PIN);

  if ((float)raw < aero.calibratedBaseline && !isCalibrating) {
    aero.calibratedBaseline -= (aero.calibratedBaseline - (float)raw) * DRIFT_SMOOTHING;
  }

  float rawAdjusted = (float)raw - (aero.calibratedBaseline / 2.0f);

  if (rawAdjusted < 0) {
    rawAdjusted = 0;
  }

  float instantAQI = (rawAdjusted / 4095.0f) * 500.0f * 1.15f;

  aero.aqi = (0.85f * aero.aqi) + (0.15f * instantAQI);
  aero.aqi = constrain(aero.aqi, 10, 500);

  aero.ci = 0.5f + (aero.aqi / 350.0f);

  updateIntelligence();
}

void updateStoragePercent() {
  size_t used = LittleFS.usedBytes();
  size_t total = LittleFS.totalBytes();

  if (total > 0) {
    aero.storage = ((float)used / (float)total) * 100.0f;
  } else {
    aero.storage = 0;
  }
}

// ---------------- STORAGE ----------------

void manageFIFO() {
  updateStoragePercent();

  if (aero.storage > 90.0 || aero.currentLines > MAX_LOG_LINES) {
    Serial.println(F("FIFO: Triggered Rotation."));

    File readF = LittleFS.open(LOG_FILE, "r");
    File tempF = LittleFS.open("/temp_rotate.csv", "w");

    if (!readF || !tempF) {
      if (readF) readF.close();
      if (tempF) tempF.close();
      return;
    }

    int linesToSkip = aero.currentLines / 4;
    int currentLine = 0;

    aero.currentLines = 0;

    while (readF.available()) {
      String line = readF.readStringUntil('\n');

      if (currentLine >= linesToSkip || currentLine == 0) {
        tempF.println(line);
        aero.currentLines++;
      }

      currentLine++;
    }

    readF.close();
    tempF.close();

    LittleFS.remove(LOG_FILE);
    LittleFS.rename("/temp_rotate.csv", LOG_FILE);

    saveMetadata();

    Serial.println(F("FIFO: Done."));
  }
}

void logData() {
  aero.logCounter++;
  aero.currentLines++;

  File f = LittleFS.open(LOG_FILE, "a");

  if (f) {
    f.printf("%lu,%.1f,%.1f,%.1f,%.2f,%s,%s\n",
             aero.logCounter,
             aero.aqi,
             aero.temp,
             aero.hum,
             aero.ci,
             aero.bact,
             aero.risk);

    f.close();

    logSyncCounter++;

    if (logSyncCounter >= 20) {
      saveMetadata();
      logSyncCounter = 0;
    }
  }
}

// ---------------- LCD HELPERS ----------------

void printSlidingLine(byte row, const char* text, int &pos) {
  char padded[120];
  char window[17];

  snprintf(padded, sizeof(padded), "%s                ", text);

  int len = strlen(padded);

  if (len <= 16) {
    snprintf(window, sizeof(window), "%-16s", text);
  } else {
    for (int i = 0; i < 16; i++) {
      int idx = (pos + i) % len;
      window[i] = padded[idx];
    }

    window[16] = '\0';

    pos++;

    if (pos >= len) {
      pos = 0;
    }
  }

  lcd.setCursor(0, row);
  lcd.print(window);
}

void updateLCD() {
  if (millis() - topScrollTimer > 350) {
    if (millis() < lcdMsgEnd) {
      printSlidingLine(0, activeLcdMsg, topScrollPos);
    } else {
      printSlidingLine(0, "AeroNode by Shrut and Siddhi", topScrollPos);
    }

    topScrollTimer = millis();
  }

  if (millis() - bottomScrollTimer > 350) {
    char data[80];

    snprintf(data, sizeof(data),
             "AQI:%.0f | T:%.1fC | H:%.0f%%",
             aero.aqi,
             aero.temp,
             aero.hum);

    printSlidingLine(1, data, bottomScrollPos);

    bottomScrollTimer = millis();
  }
}

// ---------------- WEB ENDPOINTS ----------------

void handleData() {
  updateStoragePercent();

  char json[360];

  snprintf(json, sizeof(json),
           "{\"aqi\":%.1f,\"aqiS\":\"%s\",\"temp\":%.1f,\"hum\":%.1f,\"ci\":%.2f,\"bact\":\"%s\",\"risk\":\"%s\",\"storage\":%.1f,\"records\":%lu,\"cal\":%d}",
           aero.aqi,
           aero.aqiS,
           aero.temp,
           aero.hum,
           aero.ci,
           aero.bact,
           aero.risk,
           aero.storage,
           aero.logCounter,
           isCalibrating ? 1 : 0);

  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", json);
}

void handleMessage() {
  if (!server.hasArg("msg")) {
    server.send(400, "text/plain", "Missing msg");
    return;
  }

  strncpy(activeLcdMsg, server.arg("msg").c_str(), sizeof(activeLcdMsg) - 1);
  activeLcdMsg[sizeof(activeLcdMsg) - 1] = '\0';

  topScrollPos = 0;

  int dur = server.hasArg("dur") ? server.arg("dur").toInt() : 15;

  if (dur < 1) {
    dur = 15;
  }

  lcdMsgEnd = millis() + ((unsigned long)dur * 1000UL);

  server.send(200, "text/plain", "OK");
}

void handleCalibrate() {
  if (isCalibrating) {
    server.send(200, "text/plain", "Busy");
    return;
  }

  isCalibrating = true;
  calibSamplesCount = 0;
  calibTotal = 0;
  calibLastSample = millis();

  server.send(200, "text/plain", "Started");
}

void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleCaptive() {
  server.sendHeader("Location", String("http://") + apIP.toString(), true);
  server.send(302, "text/plain", "");
}

void handleHistory() {
  File f = LittleFS.open(LOG_FILE, "r");

  if (!f) {
    server.send(404, "text/plain", "Log file not found");
    return;
  }

  server.streamFile(f, "text/csv");
  f.close();
}

void handleDownload() {
  File f = LittleFS.open(LOG_FILE, "r");

  if (!f) {
    server.send(404, "text/plain", "Log file not found");
    return;
  }

  server.sendHeader("Content-Disposition", "attachment; filename=history.csv");
  server.streamFile(f, "text/csv");
  f.close();
}

// ---------------- BOOT & LOOP ----------------

void setup() {
  Serial.begin(115200);

  Wire.begin();
  dht.begin();

  lcd.init();
  lcd.backlight();

  if (!LittleFS.begin(true)) {
    Serial.println(F("FS Failure"));
  }

  loadMetadata();
  loadCalibration();

  if (!LittleFS.exists(LOG_FILE)) {
    File f = LittleFS.open(LOG_FILE, "w");

    if (f) {
      f.println(F("ID,AQI,Temp,Hum,CI,Status,Risk"));
      f.close();

      aero.currentLines = 1;
      saveMetadata();
    }
  }

  readSensors();
  updateStoragePercent();

  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(SSID_AP, PASS_AP);

  dnsServer.start(DNS_PORT, "*", apIP);

  server.on("/", handleRoot);
  server.on("/data", HTTP_GET, handleData);
  server.on("/message", HTTP_POST, handleMessage);
  server.on("/calibrate", HTTP_POST, handleCalibrate);
  server.on("/history", HTTP_GET, handleHistory);
  server.on("/download", HTTP_GET, handleDownload);

  server.on("/generate_204", handleCaptive);
  server.on("/fwlink", handleCaptive);
  server.onNotFound(handleCaptive);

  server.begin();

  Serial.println(F("AeroNode Finalized."));
}

void loop() {
  dnsServer.processNextRequest();
  server.handleClient();

  if (isCalibrating && millis() - calibLastSample > 500) {
    calibTotal += analogRead(MQ135_PIN);
    calibSamplesCount++;
    calibLastSample = millis();

    if (calibSamplesCount >= 10) {
      aero.calibratedBaseline = (float)calibTotal / 10.0f;
      saveCalibration();
      isCalibrating = false;
    }
  }

  if (millis() - sensorTimer > 2000) {
    readSensors();
    sensorTimer = millis();
  }

  updateLCD();

  if (millis() - logTimer > 60000) {
    manageFIFO();
    logData();
    logTimer = millis();
  }
}
