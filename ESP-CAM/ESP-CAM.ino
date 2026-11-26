#include <WiFi.h>
#include <WebServer.h>
#include <esp32cam.h>
#include <Preferences.h>
#include "esp_camera.h"

// ================= КОНСТАНТЫ =================
#define BOOT_BTN_PIN      0
#define FLASH_LED_PIN     4
#define LED_PWM_CHANNEL   0
#define LED_PWM_FREQ      5000
#define LED_PWM_RES       8

#define MAX_NETWORKS      10
#define AP_SSID           "ESP32-QR-Setup"
#define AP_PASS           "12345678"
#define BOOT_HOLD_MS      3000

// ================= ГЛОБАЛЬНЫЕ =================
WebServer server(80);
Preferences prefs;

struct WifiCred {
  String ssid;
  String pass;
};

WifiCred networks[MAX_NETWORKS];
int networkCount = 0;

// Настройки камеры
int currentQuality = 12;
esp32cam::Resolution currentFrameSize = esp32cam::Resolution::find(640, 480);
int sensorBrightness = 0;
int sensorContrast = 0;
int sensorSaturation = 0;
int sensorWbMode = 0;

// Настройки LED (только в RAM, NVS не трогаем)
int ledBrightness = 128;
bool ledState = false;

struct ResolutionOption {
  const char* name;
  esp32cam::Resolution value;
};

ResolutionOption resolutionOptions[] = {
  {"QVGA (320x240)", esp32cam::Resolution::find(320, 240)},
  {"VGA (640x480)", esp32cam::Resolution::find(640, 480)},
  {"SVGA (800x600)", esp32cam::Resolution::find(800, 600)},
  {"XGA (1024x768)", esp32cam::Resolution::find(1024, 768)},
  {"HD (1280x720)", esp32cam::Resolution::find(1280, 720)},
  {"SXGA (1280x1024)", esp32cam::Resolution::find(1280, 1024)},
  {"UXGA (1600x1200)", esp32cam::Resolution::find(1600, 1200)}
};
const int numResolutions = sizeof(resolutionOptions) / sizeof(resolutionOptions[0]);

// ================= LED =================
void initLed() {
  ledcAttach(FLASH_LED_PIN, LED_PWM_FREQ, LED_PWM_RES);
  ledcWrite(FLASH_LED_PIN, 0);
}

void setLed(bool on, int brightness) {
  ledBrightness = constrain(brightness, 0, 255);
  ledState = on;
  ledcWrite(FLASH_LED_PIN, on ? ledBrightness : 0);
  // NVS не трогаем — состояние LED сугубо оперативное
}

// ================= НАСТРОЙКИ КАМЕРЫ =================
void loadCameraSettings() {
  prefs.begin("camera", true);
  currentQuality = prefs.getInt("quality", 12);
  int width = prefs.getInt("width", 640);
  int height = prefs.getInt("height", 480);

  sensorBrightness = prefs.getInt("brightness", 0);
  sensorContrast = prefs.getInt("contrast", 0);
  sensorSaturation = prefs.getInt("saturation", 0);
  sensorWbMode = prefs.getInt("wb_mode", 0);
  prefs.end();

  currentFrameSize = esp32cam::Resolution::find(width, height);
  Serial.printf("[NVS] Загружены настройки: %dx%d, сжатие JPEG=%d\n", width, height, currentQuality);
}

void saveCameraSettings() {
  prefs.begin("camera", false);
  prefs.putInt("quality", currentQuality);
  prefs.putInt("width", currentFrameSize.getWidth());
  prefs.putInt("height", currentFrameSize.getHeight());

  prefs.putInt("brightness", sensorBrightness);
  prefs.putInt("contrast", sensorContrast);
  prefs.putInt("saturation", sensorSaturation);
  prefs.putInt("wb_mode", sensorWbMode);
  prefs.end();
  Serial.println("[NVS] Настройки камеры сохранены.");
}

void applySensorSettings() {
  sensor_t * s = esp_camera_sensor_get();
  if (s) {
    s->set_brightness(s, sensorBrightness);
    s->set_contrast(s, sensorContrast);
    s->set_saturation(s, sensorSaturation);

    if (sensorWbMode == 0) {
      s->set_whitebal(s, 1);
    } else {
      s->set_whitebal(s, 0);
      s->set_wb_mode(s, sensorWbMode);
    }
    Serial.printf("[SENSOR] Яркость=%d, Контраст=%d, Насыщенность=%d, WB=%d\n",
                  sensorBrightness, sensorContrast, sensorSaturation, sensorWbMode);
  }
}

// ================= WI-FI =================
void loadNetworks() {
  prefs.begin("wifi", true);
  networkCount = prefs.getInt("count", 0);
  if (networkCount > MAX_NETWORKS) networkCount = MAX_NETWORKS;

  for (int i = 0; i < networkCount; i++) {
    networks[i].ssid = prefs.getString(("ssid" + String(i)).c_str(), "");
    networks[i].pass = prefs.getString(("pass" + String(i)).c_str(), "");
  }
  prefs.end();
}

void saveNetworks() {
  prefs.begin("wifi", false);
  prefs.putInt("count", networkCount);
  for (int i = 0; i < networkCount; i++) {
    prefs.putString(("ssid" + String(i)).c_str(), networks[i].ssid);
    prefs.putString(("pass" + String(i)).c_str(), networks[i].pass);
  }
  prefs.end();
}

bool tryConnectSaved() {
  if (networkCount == 0) return false;

  for (int i = 0; i < networkCount; i++) {
    Serial.printf("[Wi-Fi] Попытка подключения к '%s'...\n", networks[i].ssid.c_str());
    WiFi.begin(networks[i].ssid.c_str(), networks[i].pass.c_str());

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
      delay(200);
    }

    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("[Wi-Fi] Подключено! IP: %s\n", WiFi.localIP().toString().c_str());
      return true;
    }
    WiFi.disconnect();
  }
  return false;
}

bool checkBootButton() {
  pinMode(BOOT_BTN_PIN, INPUT_PULLUP);
  if (digitalRead(BOOT_BTN_PIN) != LOW) return false;

  unsigned long start = millis();
  while (digitalRead(BOOT_BTN_PIN) == LOW) {
    if (millis() - start >= BOOT_HOLD_MS) {
      Serial.println("[SYSTEM] BOOT удерживается — переход в режим AP.");
      return true;
    }
    delay(50);
  }
  return false;
}

// ================= ОБРАБОТЧИКИ =================
void handleCapture() {
  Serial.println("--- Получен запрос /capture ---");

  auto staleFrame = esp32cam::capture();
  if (staleFrame) {
    staleFrame.reset();
  }

  auto frame = esp32cam::capture();

  if (!frame) {
    Serial.println("❌ ОШИБКА: esp32cam::capture() вернула nullptr!");
    server.send(500, "text/plain", "Capture failed - check Serial Monitor");
    return;
  }

  Serial.printf("✅ Захвачен кадр: %dx%d | Размер файла: %u байт | Сжатие: %d\n",
                frame->getWidth(), frame->getHeight(), frame->size(), currentQuality);

  server.setContentLength(frame->size());
  server.send(200, "image/jpeg", "");
  frame->writeTo(server.client());
}

String getSettingsHTML() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html lang="ru">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>ESP32-CAM Настройки</title>
  <style>
    body { font-family: Arial, sans-serif; margin: 0; padding: 20px; background: #f0f2f5; }
    .card { background: #fff; padding: 20px; border-radius: 8px; box-shadow: 0 2px 4px rgba(0,0,0,0.1); margin-bottom: 20px; }
    h2 { margin-top: 0; color: #333; border-bottom: 1px solid #eee; padding-bottom: 10px; }
    .form-group { margin-bottom: 15px; }
    label { display: block; margin-bottom: 5px; font-weight: bold; font-size: 14px; }
    input[type="number"], select { width: 100%; padding: 10px; border: 1px solid #ccc; border-radius: 4px; box-sizing: border-box; }
    input[type="range"] { width: 100%; }
    button[type="submit"] { width: 100%; padding: 12px; background: #007bff; color: white; border: none; border-radius: 4px; font-size: 16px; cursor: pointer; margin-top: 10px; }
    .btn-back { display: inline-block; margin-bottom: 20px; text-decoration: none; color: #007bff; font-weight: bold; }
    .btn-capture { display: block; width: 100%; padding: 12px; background: #28a745; color: white; text-align: center; text-decoration: none; border-radius: 4px; margin-bottom: 20px; font-weight: bold; }
    .val-display { float: right; font-weight: normal; color: #007bff; }
  </style>
</head>
<body>
  <a href="/" class="btn-back">← На главную</a>
  <a href="/capture" target="_blank" class="btn-capture">📷 Сделать снимок</a>

  <form action="/settings" method="POST">

    <div class="card">
      <h2>Формат и качество</h2>
      <div class="form-group">
        <label>Разрешение:</label>
        <select name="framesize">
)rawliteral";

  for (int i = 0; i < numResolutions; i++) {
    bool selected = (resolutionOptions[i].value.getWidth() == currentFrameSize.getWidth() &&
                     resolutionOptions[i].value.getHeight() == currentFrameSize.getHeight());
    html += "<option value='" + String(i) + "'" + (selected ? " selected" : "") + ">" + resolutionOptions[i].name + "</option>";
  }

  html += R"rawliteral(
        </select>
      </div>
      <div class="form-group">
        <label>Сжатие JPEG (0-63, меньше = лучше):</label>
        <input type="number" name="quality" min="0" max="63" value=")rawliteral";
  html += String(currentQuality);
  html += R"rawliteral(">
      </div>
    </div>

    <div class="card">
      <h2>Настройки сенсора (OV2640)</h2>
      <div class="form-group">
        <label>Яркость (-2 до 2): <span class="val-display">)rawliteral";
  html += String(sensorBrightness);
  html += R"rawliteral(</span></label>
        <input type="range" name="brightness" min="-2" max="2" value=")rawliteral";
  html += String(sensorBrightness);
  html += R"rawliteral(">
      </div>
      <div class="form-group">
        <label>Контраст (-2 до 2): <span class="val-display">)rawliteral";
  html += String(sensorContrast);
  html += R"rawliteral(</span></label>
        <input type="range" name="contrast" min="-2" max="2" value=")rawliteral";
  html += String(sensorContrast);
  html += R"rawliteral(">
      </div>
      <div class="form-group">
        <label>Насыщенность (-2 до 2): <span class="val-display">)rawliteral";
  html += String(sensorSaturation);
  html += R"rawliteral(</span></label>
        <input type="range" name="saturation" min="-2" max="2" value=")rawliteral";
  html += String(sensorSaturation);
  html += R"rawliteral(">
      </div>
      <div class="form-group">
        <label>Баланс белого:</label>
        <select name="wb_mode">
          <option value="0" )rawliteral"; if(sensorWbMode == 0) html += "selected"; html += R"rawliteral(>Авто</option>
          <option value="1" )rawliteral"; if(sensorWbMode == 1) html += "selected"; html += R"rawliteral(>Солнечно (Sunny)</option>
          <option value="2" )rawliteral"; if(sensorWbMode == 2) html += "selected"; html += R"rawliteral(>Облачно (Cloudy)</option>
          <option value="3" )rawliteral"; if(sensorWbMode == 3) html += "selected"; html += R"rawliteral(>Офис (Office)</option>
          <option value="4" )rawliteral"; if(sensorWbMode == 4) html += "selected"; html += R"rawliteral(>Дом (Home)</option>
        </select>
      </div>
    </div>

    <button type="submit">Сохранить все настройки камеры</button>
  </form>

  <br><hr><br>

  <div class="card">
    <h2>Управление LED</h2>
    <p style="color:#888;font-size:13px;">Состояние LED не сохраняется в NVS — только оперативно.</p>
    <form action="/led" method="POST">
      <div class="form-group">
        <label>Яркость (0-255):</label>
        <input type="number" name="bright" min="0" max="255" value=")rawliteral";
  html += String(ledBrightness);
  html += R"rawliteral(">
      </div>
      <div class="form-group">
        <label>
          <input type="checkbox" name="state" value="on" )rawliteral";
  html += String(ledState ? "checked" : "");
  html += R"rawliteral(> Включить
        </label>
      </div>
      <button type="submit">Применить LED</button>
    </form>
  </div>
</body>
</html>
)rawliteral";
  return html;
}

void handleSettingsGet() {
  Serial.println("--- Получен запрос /settings (GET) ---");
  server.send(200, "text/html; charset=utf-8", getSettingsHTML());
}

void handleSettingsPost() {
  Serial.println("--- Получен запрос /settings (POST) ---");
  if (server.hasArg("framesize") && server.hasArg("quality")) {
    int idx = server.arg("framesize").toInt();
    int newQuality = server.arg("quality").toInt();

    if (idx >= 0 && idx < numResolutions) {
      currentFrameSize = resolutionOptions[idx].value;
    }
    currentQuality = constrain(newQuality, 0, 63);

    if (server.hasArg("brightness")) sensorBrightness = server.arg("brightness").toInt();
    if (server.hasArg("contrast")) sensorContrast = server.arg("contrast").toInt();
    if (server.hasArg("saturation")) sensorSaturation = server.arg("saturation").toInt();
    if (server.hasArg("wb_mode")) sensorWbMode = server.arg("wb_mode").toInt();

    saveCameraSettings();

    server.send(200, "text/html; charset=utf-8", "<h1>Настройки сохранены!</h1><p>Плата перезагружается...</p>");
    delay(1000);
    ESP.restart();
  } else {
    server.send(400, "text/plain", "Bad Request");
  }
}

void handleRootSta() {
  Serial.println("--- Получен запрос / (STA) ---");
  String html = R"rawliteral(
<!DOCTYPE html>
<html lang="ru">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>ESP32-CAM</title>
  <style>
    body { font-family: Arial, sans-serif; margin: 0; padding: 20px; background: #f0f2f5; display: flex; justify-content: center; align-items: center; min-height: 100vh; }
    .card { background: #fff; padding: 40px; border-radius: 12px; box-shadow: 0 4px 12px rgba(0,0,0,0.1); text-align: center; max-width: 400px; width: 100%; }
    h1 { color: #333; margin-top: 0; font-size: 24px; }
    .status { display: inline-block; padding: 6px 12px; background: #d4edda; color: #155724; border-radius: 20px; font-size: 14px; font-weight: bold; margin-bottom: 30px; }
    .btn { display: block; width: 100%; padding: 15px; margin-bottom: 15px; border: none; border-radius: 8px; font-size: 16px; font-weight: bold; cursor: pointer; text-decoration: none; box-sizing: border-box; transition: background 0.3s; }
    .btn-capture { background: #007bff; color: white; }
    .btn-capture:hover { background: #0056b3; }
    .btn-settings { background: #e4e6eb; color: #333; }
    .btn-settings:hover { background: #d0d2d6; }
  </style>
</head>
<body>
  <div class="card">
    <h1>ESP32-CAM</h1>
    <div class="status">● Подключено к Wi-Fi</div>

    <a href="/capture" target="_blank" class="btn btn-capture">📷 Сделать снимок</a>
    <a href="/settings" class="btn btn-settings">⚙️ Настройки камеры и LED</a>
  </div>
</body>
</html>
)rawliteral";
  server.send(200, "text/html; charset=utf-8", html);
}

void handleLed() {
  Serial.println("--- Получен запрос /led ---");

  // Правильно определяем on/off по значению, а не по факту наличия аргумента
  bool on = false;
  if (server.hasArg("state")) {
    String s = server.arg("state");
    s.toLowerCase();
    on = (s == "on" || s == "1" || s == "true");
  }

  // Поддерживаем оба имени параметра яркости: brightness (Python) и bright (веб-форма)
  int bright = ledBrightness;
  if (server.hasArg("brightness")) {
    bright = server.arg("brightness").toInt();
  } else if (server.hasArg("bright")) {
    bright = server.arg("bright").toInt();
  }

  setLed(on, bright);

  if (server.method() == HTTP_POST) {
    server.sendHeader("Location", "/settings");
    server.send(303);
  } else {
    String json = "{\"state\":" + String(ledState ? "true" : "false") +
                  ",\"brightness\":" + String(ledBrightness) + "}";
    server.send(200, "application/json", json);
  }
}

// ================= СТРАНИЦА AP (Wi-Fi) =================
void handleRootAp() {
  String prefilledSsid = server.hasArg("ssid") ? server.arg("ssid") : "";
  String html = R"rawliteral(
<!DOCTYPE html>
<html lang="ru">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>ESP32-CAM Setup</title>
  <style>
    body { font-family: Arial, sans-serif; margin: 0; padding: 20px; background: #f0f2f5; }
    .card { background: #fff; padding: 20px; border-radius: 8px; box-shadow: 0 2px 4px rgba(0,0,0,0.1); margin-bottom: 20px; }
    h2 { margin-top: 0; color: #333; }
    .net-item { display: flex; justify-content: space-between; align-items: center; padding: 10px; border-bottom: 1px solid #eee; }
    .btn { padding: 8px 12px; border: none; border-radius: 4px; cursor: pointer; text-decoration: none; color: white; font-size: 14px; }
    .btn-del { background: #dc3545; }
    .btn-scan { background: #17a2b8; margin-bottom: 15px; display: inline-block; }
    .btn-scan:disabled { background: #6c757d; cursor: not-allowed; }
    .form-group { margin-bottom: 15px; }
    label { display: block; margin-bottom: 5px; font-weight: bold; }
    input[type="text"], input[type="password"] { width: 100%; padding: 10px; border: 1px solid #ccc; border-radius: 4px; box-sizing: border-box; }
    button[type="submit"] { width: 100%; padding: 12px; background: #007bff; color: white; border: none; border-radius: 4px; font-size: 16px; cursor: pointer; }
    .btn-settings-link { display: block; text-align: center; margin-top: 20px; color: #007bff; text-decoration: none; font-weight: bold; }
  </style>
  <script>
    function startScan(btn) {
      btn.innerText = 'Идёт сканирование...';
      btn.disabled = true;
      setTimeout(() => { window.location.href = '/scan'; }, 100);
    }
  </script>
</head>
<body>
  <div class="card">
    <h2>Сохранённые сети ()rawliteral";
  html += String(networkCount);
  html += R"rawliteral(/10)</h2>
)rawliteral";

  if (networkCount == 0) {
    html += "<p style='color:#888;'>Нет сохраненных сетей.</p>";
  } else {
    for (int i = 0; i < networkCount; i++) {
      html += "<div class='net-item'><span>" + networks[i].ssid + "</span>";
      html += "<a href='/del?idx=" + String(i) + "' class='btn btn-del'>Удалить</a></div>";
    }
  }

  html += R"rawliteral(
  </div>
  <div class="card">
    <h2>Добавить сеть</h2>
    <button class="btn btn-scan" onclick="startScan(this)">Сканировать эфир</button>
    <form action="/add" method="POST">
      <div class="form-group">
        <label>SSID:</label>
        <input type="text" name="ssid" required value=")rawliteral";
  html += prefilledSsid;
  html += R"rawliteral(">
      </div>
      <div class="form-group">
        <label>Пароль:</label>
        <input type="password" name="pass">
      </div>
      <button type="submit">Сохранить сеть</button>
    </form>
  </div>
  <a href="/settings" class="btn-settings-link">⚙️ Настройки камеры и LED</a>
</body>
</html>
)rawliteral";
  server.send(200, "text/html; charset=utf-8", html);
}

void handleScan() {
  int n = WiFi.scanNetworks();
  String html = "<html><head><meta charset='UTF-8'><title>Сети</title></head><body>";
  html += "<a href='/'>← Назад</a><h2>Найденные сети</h2>";
  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;
    String encodedSsid = ssid;
    encodedSsid.replace("%", "%25"); encodedSsid.replace(" ", "%20"); encodedSsid.replace("#", "%23");
    html += "<div><span>" + ssid + " (" + String(WiFi.RSSI(i)) + " dBm)</span> ";
    html += "<a href='/?ssid=" + encodedSsid + "'>Выбрать</a></div><br>";
  }
  html += "</body></html>";
  server.send(200, "text/html; charset=utf-8", html);
}

void handleAdd() {
  if (server.hasArg("ssid") && networkCount < MAX_NETWORKS) {
    String ssid = server.arg("ssid");
    ssid.trim();
    if (ssid.length() > 0) {
      networks[networkCount].ssid = ssid;
      networks[networkCount].pass = server.arg("pass");
      networkCount++;
      saveNetworks();
    }
  }
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleDel() {
  if (server.hasArg("idx")) {
    int idx = server.arg("idx").toInt();
    if (idx >= 0 && idx < networkCount) {
      for (int i = idx; i < networkCount - 1; i++) networks[i] = networks[i + 1];
      networkCount--;
      saveNetworks();
    }
  }
  server.sendHeader("Location", "/");
  server.send(303);
}

// ================= ЗАПУСК =================
void startStaMode() {
  server.on("/", handleRootSta);
  server.on("/capture", handleCapture);
  server.on("/settings", HTTP_GET, handleSettingsGet);
  server.on("/settings", HTTP_POST, handleSettingsPost);
  server.on("/led", HTTP_GET, handleLed);
  server.on("/led", HTTP_POST, handleLed);
  server.begin();
}

void startApMode() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.printf("[SYSTEM] AP запущен: %s, IP: %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());

  server.on("/", handleRootAp);
  server.on("/scan", handleScan);
  server.on("/add", HTTP_POST, handleAdd);
  server.on("/del", handleDel);
  server.on("/capture", handleCapture);
  server.on("/settings", HTTP_GET, handleSettingsGet);
  server.on("/settings", HTTP_POST, handleSettingsPost);
  server.on("/led", HTTP_POST, handleLed);
  server.begin();
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n--- ESP32-CAM QR (multi-WiFi) ---");

  Serial.printf("Чип: %s, ревизия: %d, ядер: %d\n",
    ESP.getChipModel(), ESP.getChipRevision(), ESP.getChipCores());
  Serial.printf("Частота CPU: %d MHz\n", ESP.getCpuFreqMHz());
  Serial.printf("Flash: %d MB\n", ESP.getFlashChipSize() / (1024 * 1024));
  Serial.printf("PSRAM: %d KB\n", ESP.getPsramSize() / 1024);

  // LED — только RAM, NVS не трогаем. Всегда стартуем с выключенного.
  initLed();
  ledState = false;
  ledBrightness = 128;
  ledcWrite(FLASH_LED_PIN, 0);

  loadCameraSettings();
  loadNetworks();

  bool forceAp = checkBootButton();
  bool wifiReady = false;

  if (!forceAp && networkCount > 0) {
    WiFi.mode(WIFI_STA);
    if (tryConnectSaved()) {
      startStaMode();
      wifiReady = true;
    }
  }

  if (!wifiReady) {
    startApMode();
  }

  Serial.println("[CAMERA] Инициализация камеры...");
  delay(500);

  esp32cam::Config cfg;
  cfg.setPins(esp32cam::pins::AiThinker);
  cfg.setResolution(currentFrameSize);
  cfg.setJpeg(currentQuality);
  cfg.setBufferCount(2);

  if (!esp32cam::Camera.begin(cfg)) {
    Serial.println("❌ Камера не инициализирована. Перезагрузка...");
    delay(3000);
    ESP.restart();
  }

  Serial.printf("[CAMERA] ✅ Камера успешно инициализирована!\n");
  Serial.printf("[CAMERA] ✅ Фактическое разрешение: %dx%d\n",
                currentFrameSize.getWidth(), currentFrameSize.getHeight());
  Serial.printf("[CAMERA] ✅ Фактическое сжатие JPEG: %d\n", currentQuality);

  sensor_t * s = esp_camera_sensor_get();
  if (s) {
    s->set_quality(s, currentQuality);
    Serial.printf("[CAMERA] Принудительно установлено качество датчика: %d\n", currentQuality);
  }

  applySensorSettings();
  Serial.println("[SYSTEM] Система готова к работе.");
}

void loop() {
  server.handleClient();
}