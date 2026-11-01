/*
  Smart IoT Electrical Fault Detection System V3.0
  Target: ESP32 DevKit V1 (30-pin), isolated 12V DC demonstration only.

  IMPORTANT SAFETY:
  - Never connect this project to 230V AC mains.
  - Do not create a physical short circuit. Use the software test mode.
  - Verify every module's labels and output voltage before connecting ESP32 ADC pins.
  - ESP32 pins are NOT 5V tolerant.
*/
#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Preferences.h>

// ---------- Pin map (match the finalized plan) ----------
static const int PIN_CURRENT = 32;   // ACS712 OUT through a divider
static const int PIN_VIN     = 34;   // input voltage sensor S through divider
static const int PIN_VOUT    = 35;   // output voltage sensor S through divider
static const int PIN_GREEN   = 16;
static const int PIN_YELLOW  = 17;
static const int PIN_RED     = 23;
static const int PIN_BLUE    = 27;
static const int PIN_WHITE   = 14;
static const int PIN_BUZZER  = 25;   // transistor driver input, not buzzer directly
static const int PIN_RELAY   = 26;   // relay module IN; OFF by default
// Physical rocker switches are in the 12V power path, not GPIO inputs.

// ---------- Wi-Fi setup ----------
// The ESP32 starts its own access point so no router is required.
const char *AP_SSID = "SmartFault-Setup";
const char *AP_PASSWORD = "DCmonitor123"; // Change before public demonstration.

// Change this admin password before using the admin page.
const char *ADMIN_USER = "admin";
const char *ADMIN_PASSWORD = "Suraj.bh.com";

WebServer server(80);
Preferences prefs;
LiquidCrystal_I2C lcd(0x27, 20, 4); // If blank, check backpack address (often 0x3F).

// ---------- Calibration ----------
// Each analog output is divided by 2 using 10k series + 10k to GND.
// Adjust voltage calibration after comparing with a multimeter.
float voltageCalibration = 2.00f; // ADC-side voltage x divider ratio, then module ratio below
float voltageModuleRatio = 5.00f; // Typical 0-25V board ratio; VERIFY your module
float currentZeroVolts = 2.50f;  // ACS712 zero-current output at 5V supply; calibrate
float currentSensitivity = 0.100f; // ACS712 20A version: approx 100mV/A
float currentCalibration = 1.00f;

float lowVoltageThreshold = 10.5f;
float highVoltageThreshold = 14.5f;
float overloadThreshold = 0.60f; // Demonstration threshold; adjust for test load
float shortCurrentThreshold = 1.20f;
float openCurrentThreshold = 0.08f;
float openOutputVoltageThreshold = 1.0f;

// ---------- Live data ----------
float inputVoltage = 0, outputVoltage = 0, currentAmps = 0, powerWatts = 0, energyWh = 0;
String faultStatus = "Starting";
String faultSide = "None";
String lastEvent = "System boot";
unsigned long lastSampleMs = 0, lastEnergyMs = 0, lastLcdMs = 0, lastEventMs = 0;
unsigned long bootMs = 0;
bool buzzerEnabled = true;
bool alarmsEnabled = true;
bool relayEnabled = false; // Relay output remains OFF; no switching control is exposed in this student demo.
bool testMode = false;
String testFault = "Normal";
String history[12];
int historyCount = 0;
int historyIndex = 0;
int lcdPage = 0;
String candidateFault = "Starting";
unsigned long candidateFaultSince = 0;
const unsigned long FAULT_CONFIRM_MS = 1200; // require a condition to persist before changing status
float chartVout[60] = {0};
float chartCurrent[60] = {0};
int chartIndex = 0, chartCount = 0;
unsigned long lastChartMs = 0;

float readPinVolts(int pin) {
  // Average samples to reduce noise. ESP32 ADC is approximate; calibrate in practice.
  uint32_t sum = 0;
  for (int i = 0; i < 12; i++) {
    sum += analogRead(pin);
    delayMicroseconds(250);
  }
  float raw = sum / 12.0f;
  return (raw / 4095.0f) * 3.3f;
}

void addEvent(const String &message) {
  if (message == lastEvent && millis() - lastEventMs < 5000) return;
  lastEvent = message;
  lastEventMs = millis();
  history[historyIndex] = String(millis() / 1000) + "s: " + message;
  historyIndex = (historyIndex + 1) % 12;
  if (historyCount < 12) historyCount++;
}

void setRelayOff() {
  // Most common 5V relay modules are active-low. Keep the relay OFF at boot.
  // If your module is active-high, invert this logic after verifying its datasheet.
  digitalWrite(PIN_RELAY, HIGH);
}

void updateSensors() {
  // Sensor outputs pass through 10k/10k dividers; restore original signal x2.
  float vinAdc = readPinVolts(PIN_VIN) * 2.0f;
  float voutAdc = readPinVolts(PIN_VOUT) * 2.0f;
  inputVoltage = vinAdc * voltageModuleRatio * (voltageCalibration / 2.0f);
  outputVoltage = voutAdc * voltageModuleRatio * (voltageCalibration / 2.0f);

  float currentSignal = readPinVolts(PIN_CURRENT) * 2.0f; // undo 10k/10k divider
  currentAmps = ((currentSignal - currentZeroVolts) / currentSensitivity) * currentCalibration;
  if (currentAmps < 0) currentAmps = 0; // one-direction DC demonstration
  if (currentAmps > 25) currentAmps = 25; // sanity clamp, not a protection function
  powerWatts = outputVoltage * currentAmps;

  unsigned long now = millis();
  if (lastEnergyMs == 0) lastEnergyMs = now;
  float elapsedHours = (now - lastEnergyMs) / 3600000.0f;
  if (elapsedHours > 0 && elapsedHours < 0.1f) energyWh += powerWatts * elapsedHours;
  lastEnergyMs = now;
}

void diagnoseRaw() {
  if (testMode) {
    faultStatus = testFault;
    faultSide = (testFault == "Normal") ? "None" : "Software simulation";
    return;
  }
  faultStatus = "Normal";
  faultSide = "None";

  if (inputVoltage > highVoltageThreshold) {
    faultStatus = "High Voltage"; faultSide = "Input / supply";
  } else if (inputVoltage > 0.5f && inputVoltage < lowVoltageThreshold) {
    faultStatus = "Low Voltage"; faultSide = "Input / supply";
  } else if (currentAmps >= shortCurrentThreshold && outputVoltage < inputVoltage * 0.55f) {
    faultStatus = "Short Circuit Suspected"; faultSide = "Output / load";
  } else if (currentAmps >= overloadThreshold) {
    faultStatus = "Overload"; faultSide = "Output / load";
  } else if (inputVoltage > 5.0f && currentAmps < openCurrentThreshold && outputVoltage < openOutputVoltageThreshold) {
    faultStatus = "Open Circuit Suspected"; faultSide = "Output / load";
  }
  // A single total-current sensor cannot identify the exact parallel branch.
}


void diagnose() {
  String previous = faultStatus;
  diagnoseRaw();
  String detected = faultStatus;
  String detectedSide = faultSide;
  // Software simulation is immediate; physical readings use a short confirmation period.
  if (testMode) { candidateFault = detected; candidateFaultSince = millis(); return; }
  if (detected == previous) { candidateFault = detected; candidateFaultSince = millis(); return; }
  if (detected != candidateFault) { candidateFault = detected; candidateFaultSince = millis(); }
  if (millis() - candidateFaultSince >= FAULT_CONFIRM_MS) {
    faultStatus = candidateFault;
    faultSide = detectedSide;
  } else {
    faultStatus = previous;
  }
}

void updateIndicators() {
  bool normal = faultStatus == "Normal";
  bool critical = faultStatus.indexOf("Short Circuit") >= 0 || faultStatus == "High Voltage";
  bool warning = !normal && !critical && faultStatus != "Starting";
  digitalWrite(PIN_GREEN, normal ? HIGH : LOW);
  digitalWrite(PIN_YELLOW, warning ? HIGH : LOW);
  digitalWrite(PIN_RED, critical ? HIGH : LOW);
  digitalWrite(PIN_BLUE, WiFi.softAPIP().toString() == "0.0.0.0" ? LOW : HIGH);
  digitalWrite(PIN_WHITE, testMode ? HIGH : LOW);
  // Use a transistor driver appropriate for the buzzer voltage/current.
  digitalWrite(PIN_BUZZER, (alarmsEnabled && buzzerEnabled && (warning || critical)) ? HIGH : LOW);
  // Keep relay OFF by default; this code does not use it for automatic isolation.
  if (!relayEnabled) setRelayOff();
}

String esc(const String &s) {
  String o = s; o.replace("&", "&amp;"); o.replace("<", "&lt;"); o.replace(">", "&gt;");
  o.replace("\"", "&quot;"); o.replace("'", "&#39;"); return o;
}

String dashboardHtml() {
  String h = R"HTML(<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1"><title>Smart IoT Fault System</title><style>
  :root{color-scheme:dark}*{box-sizing:border-box}body{margin:0;background:#0b1020;color:#e9eefb;font:15px system-ui,Arial}header{padding:20px;background:#111a30;border-bottom:1px solid #28334d}h1{font-size:22px;margin:0 0 5px}.muted{color:#9eacc7;font-size:13px}.wrap{max-width:1050px;margin:auto;padding:16px}.nav{display:flex;gap:8px;flex-wrap:wrap;margin:14px 0}.nav button,button{background:#273657;color:white;border:1px solid #42577f;border-radius:10px;padding:10px 13px;cursor:pointer}.nav button.active{background:#345c9c}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(145px,1fr));gap:12px}.card{background:#141e34;border:1px solid #283653;border-radius:14px;padding:15px}.label{font-size:12px;color:#a8b6d0}.value{font-size:25px;font-weight:700;margin-top:7px;overflow-wrap:anywhere}.status{font-size:20px;font-weight:700}.pill{display:inline-block;padding:5px 9px;border-radius:99px;background:#26334e;color:#d8e6ff;font-size:12px}.section{display:none}.section.active{display:block}.row{display:flex;gap:10px;flex-wrap:wrap;align-items:center;margin:8px 0}input,select{background:#0c1427;color:#eef;border:1px solid #405171;padding:10px;border-radius:8px;max-width:100%}pre{white-space:pre-wrap;overflow-wrap:anywhere;color:#c9d8f3}.warn{color:#ffd37d}.danger{color:#ff8585}footer{color:#8b9bb8;font-size:12px;padding:20px 0}</style></head><body><header><h1>Smart IoT Electrical Fault Detection System</h1><div class="muted">Version 3.0 · 12V DC demonstration · local ESP32 dashboard</div></header><main class="wrap"><div class="nav"><button class="active" onclick="showTab('monitor',this)">Monitoring</button><button onclick="showTab('about',this)">About Project</button><button onclick="showTab('admin',this)">Admin Panel</button></div>
  <section id="monitor" class="section active"><div class="grid"><div class="card"><div class="label">Input Voltage</div><div class="value"><span id="vin">--</span> V</div></div><div class="card"><div class="label">Output Voltage</div><div class="value"><span id="vout">--</span> V</div></div><div class="card"><div class="label">Total Current</div><div class="value"><span id="amps">--</span> A</div></div><div class="card"><div class="label">Estimated Power</div><div class="value"><span id="watts">--</span> W</div></div><div class="card"><div class="label">Estimated Energy</div><div class="value"><span id="wh">--</span> Wh</div></div></div><div class="card" style="margin-top:12px"><div class="label">Fault Status</div><div id="status" class="status">Connecting…</div><p>Indicated section: <b id="side">--</b></p><span id="mode" class="pill">Live sensor mode</span></div><div class="card" style="margin-top:12px"><h3>Live Trend (last 60 samples)</h3><canvas id="trend" width="900" height="220" style="width:100%;height:auto;background:#0c1427;border-radius:10px"></canvas><div class="muted">Output voltage and current trend. Visual trend is for demonstration, not a protection instrument.</div></div><div class="card" style="margin-top:12px"><h3>Recent Events</h3><pre id="history">Loading…</pre></div></section>
  <section id="about" class="section"><div class="card"><h2>About This Project</h2><p>This student-built system monitors an isolated 12V DC test circuit using an ESP32, one current sensor and two voltage sensors. It estimates voltage, current, power and energy and displays suspected abnormal conditions.</p><h3>Objectives</h3><ul><li>Monitor input and output voltage.</li><li>Monitor total load current.</li><li>Indicate suspected open circuit, overload, low/high voltage and short-circuit-like conditions.</li><li>Provide local LCD and Wi-Fi dashboard.</li></ul><h3>Important limitations</h3><p>Fault detection is indicative, not certified protection. One current sensor cannot identify which parallel branch failed. Power factor is not measured for this DC project. Energy is an estimate.</p><h3>Team</h3><p>Soupitk Santra · Suraj Datta · Prabhas Bhunia · Devkrishna Kanta Samai · Surajit Samai</p><p>Approximate project budget: ₹4,500.</p></div></section>
  <section id="admin" class="section"><div class="card"><h2>Admin Panel</h2><p class="muted">Browser will ask for the admin username and password.</p><button onclick="loadAdmin()">Unlock / Load Settings</button><div id="adminBox" style="margin-top:14px"></div></div></section>
  <footer>Use only with an isolated 12V DC supply. Never connect to household AC mains. Do not physically short the supply.</footer></main><script>
  function showTab(id,b){document.querySelectorAll('.section').forEach(x=>x.classList.remove('active'));document.getElementById(id).classList.add('active');document.querySelectorAll('.nav button').forEach(x=>x.classList.remove('active'));b.classList.add('active')}
  async function update(){try{let r=await fetch('/api');let d=await r.json();['vin','vout','amps','watts','wh'].forEach(k=>document.getElementById(k).textContent=Number(d[k]).toFixed(2));let s=document.getElementById('status');s.textContent=d.status+' — '+d.side;s.className='status '+(d.status==='Normal'?'':(d.status.includes('Short')||d.status.includes('High')?'danger':'warn'));document.getElementById('mode').textContent=d.testMode?'SOFTWARE TEST MODE: '+d.testFault:'Live sensor mode';document.getElementById('history').textContent=d.history.join('\n')||'No events yet';drawTrend(d.trendVout,d.trendCurrent);}catch(e){document.getElementById('status').textContent='Connection lost';}}
  function drawTrend(v,c){const canvas=document.getElementById('trend');if(!canvas)return;const ctx=canvas.getContext('2d');const w=canvas.width,h=canvas.height;ctx.clearRect(0,0,w,h);ctx.strokeStyle='#2b3b59';ctx.lineWidth=1;for(let i=1;i<5;i++){ctx.beginPath();ctx.moveTo(0,h*i/5);ctx.lineTo(w,h*i/5);ctx.stroke()}function line(a,max){if(!a||a.length<2)return;ctx.beginPath();ctx.strokeStyle='#72b7ff';a.forEach((x,i)=>{let px=i*w/(a.length-1),py=h-12-Math.max(0,Math.min(1,x/(max||1)))*(h-24);if(i===0)ctx.moveTo(px,py);else ctx.lineTo(px,py)});ctx.stroke()}line(v,16);ctx.strokeStyle='#ffc66d';if(c&&c.length>1){ctx.beginPath();c.forEach((x,i)=>{let px=i*w/(c.length-1),py=h-12-Math.max(0,Math.min(1,x/3))*(h-24);if(i===0)ctx.moveTo(px,py);else ctx.lineTo(px,py)});ctx.stroke()}}
  async function loadAdmin(){try{let r=await fetch('/admin/settings');if(!r.ok)throw Error('Authentication failed');let d=await r.json();document.getElementById('adminBox').innerHTML=`<div class="row">Low V <input id="low" type="number" step="0.1" value="${d.low}"> High V <input id="high" type="number" step="0.1" value="${d.high}"></div><div class="row">Overload A <input id="over" type="number" step="0.05" value="${d.over}"> Short suspected A <input id="short" type="number" step="0.1" value="${d.short}"></div><div class="row">Voltage module ratio <input id="ratio" type="number" step="0.1" value="${d.ratio}"> Voltage calibration <input id="vcal" type="number" step="0.01" value="${d.vcal}"></div><div class="row">Current zero V <input id="zero" type="number" step="0.01" value="${d.zero}"> Current sensitivity V/A <input id="sens" type="number" step="0.001" value="${d.sens}"></div><div class="row"><label><input id="alarm" type="checkbox" ${d.alarm?'checked':''}> Alarms enabled</label><label><input id="buzz" type="checkbox" ${d.buzz?'checked':''}> Buzzer enabled</label></div><div class="row"><select id="test"><option>Normal</option><option>Low Voltage</option><option>High Voltage</option><option>Overload</option><option>Open Circuit Suspected</option><option>Short Circuit Suspected</option></select><button onclick="setTest()">Apply software test</button><button onclick="saveSettings()">Save settings</button><button onclick="resetEnergy()">Reset energy</button><button onclick="restartBoard()">Restart ESP32</button><button onclick="downloadEvents()">Export event log CSV</button></div><p class="muted">Use software test mode only; it does not create a physical electrical fault.</p>`;}catch(e){document.getElementById('adminBox').textContent='Could not load settings. Check credentials and try again.';}}
  async function saveSettings(){let p={low:+low.value,high:+high.value,over:+over.value,short:+short.value,ratio:+ratio.value,vcal:+vcal.value,zero:+zero.value,sens:+sens.value,alarm:alarm.checked,buzz:buzz.checked};let r=await fetch('/admin/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(p)});alert(r.ok?'Settings saved':'Save failed');}
  async function setTest(){let r=await fetch('/admin/test?fault='+encodeURIComponent(test.value),{method:'POST'});alert(r.ok?'Test mode updated':'Update failed');}
  async function resetEnergy(){let r=await fetch('/admin/energy-reset',{method:'POST'});alert(r.ok?'Energy counter reset':'Reset failed');}
  async function restartBoard(){if(confirm('Restart ESP32?')){await fetch('/admin/restart',{method:'POST'});}}
  function downloadEvents(){window.location.href='/admin/events.csv';}
  setInterval(update,1000);update();
  </script></body></html>)HTML";
  return h;
}

bool requireAdmin() {
  if (!server.authenticate(ADMIN_USER, ADMIN_PASSWORD)) {
    server.requestAuthentication(BASIC_AUTH, "SmartFault Admin", "Admin login required");
    return false;
  }
  return true;
}

String historyJson() {
  String s = "[";
  for (int i = 0; i < historyCount; i++) {
    int idx = (historyIndex - historyCount + i + 12) % 12;
    if (i) s += ",";
    s += "\"" + esc(history[idx]) + "\"";
  }
  s += "]";
  return s;
}

void sendApi() {
  String j = "{";
  j += "\"vin\":" + String(inputVoltage, 3) + ",\"vout\":" + String(outputVoltage, 3);
  j += ",\"amps\":" + String(currentAmps, 3) + ",\"watts\":" + String(powerWatts, 3) + ",\"wh\":" + String(energyWh, 4);
  j += ",\"status\":\"" + esc(faultStatus) + "\",\"side\":\"" + esc(faultSide) + "\"";
  j += ",\"testMode\":" + String(testMode ? "true" : "false") + ",\"testFault\":\"" + esc(testFault) + "\"";
  j += ",\"history\":" + historyJson();
  j += ",\"trendVout\":[";
  for (int i = 0; i < chartCount; i++) { if (i) j += ","; int idx = (chartIndex - chartCount + i + 60) % 60; j += String(chartVout[idx], 2); }
  j += "],\"trendCurrent\":[";
  for (int i = 0; i < chartCount; i++) { if (i) j += ","; int idx = (chartIndex - chartCount + i + 60) % 60; j += String(chartCurrent[idx], 2); }
  j += "]}";
  server.send(200, "application/json", j);
}

void sendAdminSettings() {
  if (!requireAdmin()) return;
  String j = "{";
  j += "\"low\":" + String(lowVoltageThreshold, 2) + ",\"high\":" + String(highVoltageThreshold, 2);
  j += ",\"over\":" + String(overloadThreshold, 2) + ",\"short\":" + String(shortCurrentThreshold, 2);
  j += ",\"ratio\":" + String(voltageModuleRatio, 2) + ",\"vcal\":" + String(voltageCalibration, 2);
  j += ",\"zero\":" + String(currentZeroVolts, 3) + ",\"sens\":" + String(currentSensitivity, 3);
  j += ",\"alarm\":" + String(alarmsEnabled ? "true" : "false") + ",\"buzz\":" + String(buzzerEnabled ? "true" : "false") + "}";
  server.send(200, "application/json", j);
}

void handleAdminSave() {
  if (!requireAdmin()) return;
  String body = server.arg("plain");
  auto getNum = [&](const char *key, float current) -> float {
    String needle = String("\"") + key + "\":";
    int p = body.indexOf(needle); if (p < 0) return current;
    p += needle.length(); int e = p;
    while (e < (int)body.length() && String("0123456789.-").indexOf(body[e]) >= 0) e++;
    return body.substring(p, e).toFloat();
  };
  lowVoltageThreshold = constrain(getNum("low", lowVoltageThreshold), 0.0f, 12.0f);
  highVoltageThreshold = constrain(getNum("high", highVoltageThreshold), 12.1f, 25.0f);
  overloadThreshold = constrain(getNum("over", overloadThreshold), 0.05f, 2.0f);
  shortCurrentThreshold = constrain(getNum("short", shortCurrentThreshold), overloadThreshold, 3.0f);
  voltageModuleRatio = constrain(getNum("ratio", voltageModuleRatio), 0.1f, 10.0f);
  voltageCalibration = constrain(getNum("vcal", voltageCalibration), 0.1f, 5.0f);
  currentZeroVolts = constrain(getNum("zero", currentZeroVolts), 0.5f, 3.0f);
  currentSensitivity = constrain(getNum("sens", currentSensitivity), 0.02f, 0.2f);
  alarmsEnabled = body.indexOf("\"alarm\":true") >= 0;
  buzzerEnabled = body.indexOf("\"buzz\":true") >= 0;
  prefs.begin("faultsys", false);
  prefs.putFloat("low", lowVoltageThreshold); prefs.putFloat("high", highVoltageThreshold);
  prefs.putFloat("over", overloadThreshold); prefs.putFloat("short", shortCurrentThreshold);
  prefs.putFloat("ratio", voltageModuleRatio); prefs.putFloat("vcal", voltageCalibration);
  prefs.putFloat("zero", currentZeroVolts); prefs.putFloat("sens", currentSensitivity);
  prefs.putBool("alarm", alarmsEnabled); prefs.putBool("buzz", buzzerEnabled); prefs.end();
  addEvent("Admin settings updated");
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleAdminTest() {
  if (!requireAdmin()) return;
  String f = server.arg("fault");
  const String allowed[] = {"Normal", "Low Voltage", "High Voltage", "Overload", "Open Circuit Suspected", "Short Circuit Suspected"};
  bool valid = false;
  for (auto &x : allowed) if (f == x) valid = true;
  if (!valid) { server.send(400, "text/plain", "Invalid fault"); return; }
  testMode = (f != "Normal"); testFault = f;
  addEvent(testMode ? "Software test: " + f : "Software test stopped");
  server.send(200, "application/json", "{\"ok\":true}");
}

void updateLCD() {
  if (millis() - lastLcdMs < 2500) return;
  lastLcdMs = millis();
  lcd.clear();
  if (millis() - bootMs < 10000) {
    lcd.setCursor(0,0); lcd.print("Smart IoT Fault V3.0");
    lcd.setCursor(0,1);
    if (millis() - bootMs < 2000) lcd.print("Welcome to System");
    else if (millis() - bootMs < 5000) { lcd.print("Countdown: "); lcd.print(3 - ((millis() - bootMs - 2000) / 1000)); }
    else if (millis() - bootMs < 8000) lcd.print("Initializing...");
    else lcd.print("WiFi: AP Ready");
    lcd.setCursor(0,2); lcd.print(AP_SSID);
    lcd.setCursor(0,3); lcd.print(WiFi.softAPIP().toString());
    return;
  }
  if (lcdPage == 0) {
    lcd.setCursor(0,0); lcd.print("In:"); lcd.print(inputVoltage,1); lcd.print("V Out:"); lcd.print(outputVoltage,1); lcd.print("V");
    lcd.setCursor(0,1); lcd.print("Current: "); lcd.print(currentAmps,2); lcd.print(" A");
    lcd.setCursor(0,2); lcd.print("Power: "); lcd.print(powerWatts,1); lcd.print(" W");
    lcd.setCursor(0,3); lcd.print("Energy: "); lcd.print(energyWh,2); lcd.print(" Wh");
  } else {
    lcd.setCursor(0,0); lcd.print("FAULT STATUS:");
    lcd.setCursor(0,1); lcd.print(faultStatus.substring(0,20));
    lcd.setCursor(0,2); lcd.print("Side:"); lcd.print(faultSide.substring(0,14));
    lcd.setCursor(0,3); lcd.print(WiFi.softAPIP().toString());
  }
  lcdPage = 1 - lcdPage;
}

void handleAdminEventsCsv() {
  if (!requireAdmin()) return;
  String csv = "uptime,event\r\n";
  for (int i = 0; i < historyCount; i++) {
    int idx = (historyIndex - historyCount + i + 12) % 12;
    String line = history[idx];
    int sep = line.indexOf("s: ");
    String uptime = sep >= 0 ? line.substring(0, sep) : "";
    String event = sep >= 0 ? line.substring(sep + 3) : line;
    event.replace("\"", "\"\"");
    csv += uptime + ",\"" + event + "\"\r\n";
  }
  server.sendHeader("Content-Disposition", "attachment; filename=smart_fault_events.csv");
  server.send(200, "text/csv; charset=utf-8", csv);
}

void setup() {
  bootMs = millis();
  Serial.begin(115200);
  pinMode(PIN_GREEN, OUTPUT); pinMode(PIN_YELLOW, OUTPUT); pinMode(PIN_RED, OUTPUT);
  pinMode(PIN_BLUE, OUTPUT); pinMode(PIN_WHITE, OUTPUT); pinMode(PIN_BUZZER, OUTPUT); pinMode(PIN_RELAY, OUTPUT);
  digitalWrite(PIN_GREEN, LOW); digitalWrite(PIN_YELLOW, LOW); digitalWrite(PIN_RED, LOW);
  digitalWrite(PIN_BLUE, LOW); digitalWrite(PIN_WHITE, LOW); digitalWrite(PIN_BUZZER, LOW); setRelayOff();
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_CURRENT, ADC_11db);
  analogSetPinAttenuation(PIN_VIN, ADC_11db);
  analogSetPinAttenuation(PIN_VOUT, ADC_11db);
  Wire.begin(21, 22);
  lcd.init(); lcd.backlight(); lcd.clear(); lcd.setCursor(0,0); lcd.print("Smart IoT Fault V3.0"); lcd.setCursor(0,1); lcd.print("Starting...");

  prefs.begin("faultsys", true);
  lowVoltageThreshold = prefs.getFloat("low", lowVoltageThreshold);
  highVoltageThreshold = prefs.getFloat("high", highVoltageThreshold);
  overloadThreshold = prefs.getFloat("over", overloadThreshold);
  shortCurrentThreshold = prefs.getFloat("short", shortCurrentThreshold);
  voltageModuleRatio = prefs.getFloat("ratio", voltageModuleRatio);
  voltageCalibration = prefs.getFloat("vcal", voltageCalibration);
  currentZeroVolts = prefs.getFloat("zero", currentZeroVolts);
  currentSensitivity = prefs.getFloat("sens", currentSensitivity);
  alarmsEnabled = prefs.getBool("alarm", alarmsEnabled); buzzerEnabled = prefs.getBool("buzz", buzzerEnabled);
  prefs.end();

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  Serial.print("Dashboard IP: "); Serial.println(WiFi.softAPIP());
  addEvent("Access point started at " + WiFi.softAPIP().toString());

  server.on("/", HTTP_GET, [](){ server.send(200, "text/html; charset=utf-8", dashboardHtml()); });
  server.on("/api", HTTP_GET, sendApi);
  server.on("/admin/settings", HTTP_GET, sendAdminSettings);
  server.on("/admin/settings", HTTP_POST, handleAdminSave);
  server.on("/admin/test", HTTP_POST, handleAdminTest);
  server.on("/admin/energy-reset", HTTP_POST, [](){ if(!requireAdmin()) return; energyWh=0; addEvent("Energy counter reset"); server.send(200,"application/json","{\"ok\":true}"); });
  server.on("/admin/restart", HTTP_POST, [](){ if(!requireAdmin()) return; server.send(200,"text/plain","Restarting"); delay(250); ESP.restart(); });
  server.on("/admin/events.csv", HTTP_GET, handleAdminEventsCsv);
  server.onNotFound([](){ server.send(404, "text/plain", "Not found"); });
  server.begin();
}

void loop() {
  server.handleClient();
  if (millis() - lastSampleMs >= 500) {
    lastSampleMs = millis();
    updateSensors();
    if (millis() - lastChartMs >= 1000) {
      lastChartMs = millis();
      chartVout[chartIndex] = outputVoltage;
      chartCurrent[chartIndex] = currentAmps;
      chartIndex = (chartIndex + 1) % 60;
      if (chartCount < 60) chartCount++;
    }
    String previous = faultStatus;
    diagnose();
    if (faultStatus != previous) addEvent("Status: " + faultStatus);
    updateIndicators();
  }
  updateLCD();
  delay(2);
}
