#include <M5Unified.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <time.h>
#include <vector>
#include <ArduinoJson.h>
#include <LittleFS.h>

// =======================
// CONFIG / CONSTANTS
// =======================

#define WIFI_FILE     "/wifi.json"
#define SETTINGS_FILE "/settings.json"

constexpr int  MARGIN                  = 20;
constexpr int  QUOTE_Y                 = 120;
constexpr int  CLOCK_Y                 = 20;
constexpr int  FOOTER_BOTTOM_OFFSET    = 40;
constexpr int  LINE_SPACING            = 6;

constexpr int  LONG_PRESS_MS           = 3000;
constexpr int  PORTAL_LONG_PRESS_MS    = 2000;
constexpr unsigned long PORTAL_TIMEOUT_MS       = 5UL * 60UL * 1000UL;        // 5 min
constexpr unsigned long NTP_RESYNC_INTERVAL_MS  = 6UL * 3600UL * 1000UL;      // 6 h

constexpr int  WIFI_CONNECT_RETRIES    = 40;   // ~20 s
constexpr int  NTP_RETRIES             = 60;   // ~30 s

constexpr byte DNS_PORT                = 53;

// =======================
// TYPES
// =======================

struct Quote {
    String time;
    String quote_first;
    String quote_time_case;
    String quote_last;
    String book;
    String author;
};

struct Settings {
    int  timezoneOffset;   // hours, e.g. -2, 0, +1
    bool nightMode;
};

// =======================
// GLOBALS
// =======================

WebServer  server(80);
DNSServer  dnsServer;

int W;
int H;

unsigned long touchStart = 0;
bool          touching   = false;

// =======================
// FORWARD DECLARATIONS
// =======================

void connectWiFi();
void syncTime();
void startPortal();
void showQuote(const Quote& q, const struct tm& timeinfo);
std::vector<Quote> loadQuotes(const String& timeStr);

// =======================
// SETTINGS STORAGE
// =======================

bool loadSettings(Settings& s) {
    if (!LittleFS.exists(SETTINGS_FILE)) {
        s.timezoneOffset = 0;
        s.nightMode      = false;
        return false;
    }

    File f = LittleFS.open(SETTINGS_FILE, "r");
    if (!f) return false;

    JsonDocument doc;
    auto err = deserializeJson(doc, f);
    f.close();
    if (err) return false;

    s.timezoneOffset = doc["tz"]    | 0;
    s.nightMode      = doc["night"] | false;
    return true;
}

void saveSettings(const Settings& s) {
    File f = LittleFS.open(SETTINGS_FILE, "w");
    if (!f) return;

    JsonDocument doc;
    doc["tz"]    = s.timezoneOffset;
    doc["night"] = s.nightMode;

    serializeJson(doc, f);
    f.close();
}

// =======================
// WIFI STORAGE
// =======================

bool loadWiFi(String& ssid, String& pass) {
    if (!LittleFS.exists(WIFI_FILE)) return false;

    File f = LittleFS.open(WIFI_FILE);
    if (!f) return false;

    JsonDocument doc;
    auto err = deserializeJson(doc, f);
    f.close();
    if (err) return false;

    ssid = doc["ssid"].as<String>();
    pass = doc["password"].as<String>();
    return true;
}

void saveWiFi(const String& ssid, const String& pass) {
    File f = LittleFS.open(WIFI_FILE, "w");
    if (!f) return;

    JsonDocument doc;
    doc["ssid"]     = ssid;
    doc["password"] = pass;

    serializeJson(doc, f);
    f.close();
}

// =======================
// WIFI SETUP PORTAL
// =======================

void startPortal() {
    server.stop();
    WiFi.softAP("LiteratureClockSetup", "12345678");

    touching   = false;
    touchStart = 0;

    IPAddress IP = WiFi.softAPIP();

    // Captive-portal DNS: redirect everything to us
    dnsServer.start(DNS_PORT, "*", IP);

    M5.Display.clear();
    M5.Display.setCursor(40, 200);
    M5.Display.println("WiFi Setup Mode\n");
    M5.Display.println("Connect to:");
    M5.Display.println("LiteratureClockSetup\n");
    M5.Display.println("Open browser:");
    M5.Display.println(IP.toString());
    M5.Display.display();

    server.on("/", []() {
        Settings s;
        loadSettings(s);

        String page =
            "<html><body>"
            "<h2>Literature Clock Setup</h2>"
            "<form action='/save' method='POST'>"
            "SSID:<br><input name='ssid'><br>"
            "Password:<br><input name='pass' type='password'><br><br>"
            "Timezone offset (hours):<br>"
            "<input name='tz' value='" + String(s.timezoneOffset) + "'><br><br>"
            "Night mode: "
            "<input type='checkbox' name='night' " +
            String(s.nightMode ? "checked" : "") + "><br><br>"
            "<input type='submit'>"
            "</form>"
            "</body></html>";

        server.send(200, "text/html", page);
    });

    server.on("/save", HTTP_POST, []() {
        String ssid = server.arg("ssid");
        String pass = server.arg("pass");

        Settings s;
        s.timezoneOffset = constrain(server.arg("tz").toInt(), -12, 14);
        s.nightMode      = server.hasArg("night");

        saveWiFi(ssid, pass);
        saveSettings(s);

        server.send(200, "text/html", "Saved! Rebooting...");
        delay(1000);

        server.stop();
        dnsServer.stop();
        WiFi.softAPdisconnect(true);

        ESP.restart();
    });

    server.begin();

    unsigned long portalStart = millis();

    while (true) {
        dnsServer.processNextRequest();
        server.handleClient();
        M5.update();

        // Touch-and-hold to exit portal
        if (M5.Touch.getCount() > 0) {
            if (!touching) {
                touching   = true;
                touchStart = millis();
            }
            if (millis() - touchStart > PORTAL_LONG_PRESS_MS) {
                ESP.restart();
            }
        } else {
            touching = false;
        }

        if (millis() - portalStart > PORTAL_TIMEOUT_MS) {
            ESP.restart();
        }

        delay(10);
    }
}

// =======================
// WIFI CONNECT
// =======================

void connectWiFi() {
    String ssid, pass;

    if (!loadWiFi(ssid, pass)) {
        startPortal();
    }

    M5.Display.clear();
    M5.Display.println("Connecting to:");
    M5.Display.println(ssid);
    M5.Display.display();

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        M5.Display.print(".");
        M5.Display.display();

        if (++attempts > WIFI_CONNECT_RETRIES) {
            M5.Display.println("\nFailed!");
            M5.Display.display();
            delay(2000);
            startPortal();
        }
    }

    M5.Display.println("\nConnected!");
    M5.Display.println(WiFi.localIP());
    M5.Display.display();
    delay(1000);
}

// =======================
// TIME SYNC
// =======================

void syncTime() {
    M5.Display.println("Syncing time...");
    M5.Display.display();

    Settings settings;
    loadSettings(settings);

    long offsetSeconds = settings.timezoneOffset * 3600L;
    configTime(offsetSeconds, 0, "time.google.com", "pool.ntp.org");

    struct tm timeinfo;
    int retries = 0;

    while (true) {
        if (getLocalTime(&timeinfo)) {
            int year = timeinfo.tm_year + 1900;
            if (year >= 2024 && year <= 2035) break;
        }

        delay(500);
        M5.Display.print(".");
        M5.Display.display();

        if (++retries > NTP_RETRIES) {
            M5.Display.println("\nNTP FAILED");
            M5.Display.display();
            return;
        }
    }

    M5.Display.println("\nTime OK");
    M5.Display.display();
}

// =======================
// TEXT HELPERS
// =======================

String cleanText(String s) {
    s.replace("“",     "\"");
    s.replace("”",     "\"");
    s.replace("’",     "'");
    s.replace("—",     "-");
    s.replace("–",     "-");
    s.replace("…",     "...");
    s.replace("<br/>",  " ");
    s.replace("<br>",   " ");
    s.replace("<br />", " ");

    for (size_t i = 0; i < s.length(); i++) {
        if ((uint8_t)s[i] < 32 && s[i] != '\n') {
            s[i] = ' ';
        }
    }
    return s;
}

// =======================
// QUOTE LOADING
// =======================

std::vector<Quote> loadQuotes(const String& timeStr) {
    std::vector<Quote> result;

    String path = "/" + timeStr;
    path.replace(":", "_");
    path += ".json";

    if (!LittleFS.exists(path)) return result;

    File file = LittleFS.open(path, "r");
    if (!file) return result;

    JsonDocument doc;
    auto err = deserializeJson(doc, file);
    file.close();
    if (err) return result;

    JsonArray arr = doc.as<JsonArray>();
    for (JsonObject obj : arr) {
        Quote q;
        q.time            = obj["time"].as<String>();
        q.quote_first     = cleanText(obj["quote_first"].as<String>());
        q.quote_time_case = cleanText(obj["quote_time_case"].as<String>());
        q.quote_last      = cleanText(obj["quote_last"].as<String>());
        q.book            = obj["title"].as<String>();
        q.author          = obj["author"].as<String>();

        result.push_back(q);
    }

    return result;
}

// =======================
// QUOTE RENDERING
// =======================

// Render one wrapped line, bolding the highlight phrase if present.
static void renderLine(const String& line, const String& highlight, int x, int y) {
    int pos = (highlight.length() > 0) ? line.indexOf(highlight) : -1;

    if (pos >= 0) {
        String before = line.substring(0, pos);
        String after  = line.substring(pos + highlight.length());

        M5.Display.setFont(&fonts::FreeSans18pt7b);
        M5.Display.setCursor(x, y);
        M5.Display.print(before);
        x += M5.Display.textWidth(before);

        M5.Display.setFont(&fonts::FreeSansBold18pt7b);
        M5.Display.setCursor(x, y);
        M5.Display.print(highlight);
        x += M5.Display.textWidth(highlight);

        M5.Display.setFont(&fonts::FreeSans18pt7b);
        M5.Display.setCursor(x, y);
        M5.Display.print(after);
    } else {
        M5.Display.setFont(&fonts::FreeSans18pt7b);
        M5.Display.setCursor(x, y);
        M5.Display.print(line);
    }
}

void drawQuoteWrapped(const Quote& q, int yStart) {
    int y        = yStart;
    int maxWidth = W - 2 * MARGIN;

    String text = q.quote_first + q.quote_time_case + q.quote_last;

    // Split into space-separated tokens (preserves trailing space)
    std::vector<String> words;
    String word = "";
    for (size_t i = 0; i <= text.length(); i++) {
        char c = (i < text.length()) ? text[i] : ' ';
        if (c == ' ') {
            if (word.length() > 0) {
                words.push_back(word + " ");
                word = "";
            }
        } else {
            word += c;
        }
    }

    M5.Display.setFont(&fonts::FreeSans18pt7b);

    String line = "";
    for (const String& w : words) {
        if (M5.Display.textWidth(line + w) > maxWidth && line.length() > 0) {
            renderLine(line, q.quote_time_case, MARGIN, y);
            y += M5.Display.fontHeight() + LINE_SPACING;
            line = w;
        } else {
            line += w;
        }
    }

    if (line.length() > 0) {
        renderLine(line, q.quote_time_case, MARGIN, y);
    }
}

void showQuote(const Quote& q, const struct tm& timeinfo) {
    M5.Display.clear();

    // ---- Clock ----
    char clockStr[10];
    strftime(clockStr, sizeof(clockStr), "%I:%M %p", &timeinfo);

    M5.Display.setFont(&fonts::FreeSansBold12pt7b);
    int clockW = M5.Display.textWidth(clockStr);
    int clockX = (W - clockW) / 2;
    M5.Display.setCursor(clockX, CLOCK_Y);
    M5.Display.println(clockStr);

    // ---- Quote ----
    M5.Display.setTextWrap(true);
    drawQuoteWrapped(q, QUOTE_Y);

    // ---- Footer ----
    String footer = "- " + q.book + ", " + q.author;
    M5.Display.setFont(&fonts::FreeSans9pt7b);
    int fw      = M5.Display.textWidth(footer);
    int footerX = W - fw - MARGIN;
    int footerY = H - FOOTER_BOTTOM_OFFSET;
    M5.Display.setCursor(footerX, footerY);
    M5.Display.println(footer);

    M5.Display.display();
}

// =======================
// SETUP
// =======================

void setup() {
    auto cfg = M5.config();
    M5.begin(cfg);
    M5.Display.setRotation(1);

    if (!LittleFS.begin(false)) {
        M5.Display.println("LittleFS FAIL");
        while (true) { delay(1000); }
    }

    // Quick file listing for debugging
    File root = LittleFS.open("/");
    File file = root.openNextFile();
    M5.Display.println("FILES:");
    M5.Display.display();

    int count = 0;
    while (file && count < 10) {
        M5.Display.println(file.name());
        file = root.openNextFile();
        count++;
    }

    connectWiFi();
    syncTime();
    WiFi.mode(WIFI_OFF);   // power down radio without erasing creds

    W = M5.Display.width();
    H = M5.Display.height();
    randomSeed(micros());
}

// =======================
// LOOP
// =======================

void loop() {
    // ---- Touch (long-press to enter setup) ----
    M5.update();
    auto t = M5.Touch.getDetail();
    if (t.isPressed()) {
        if (!touching) {
            touching   = true;
            touchStart = millis();
        }
        if (millis() - touchStart > LONG_PRESS_MS) {
            touching = false;
            startPortal();
        }
    } else {
        touching = false;
    }

    // ---- Time ----
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) return;

    char timeStr[6];
    strftime(timeStr, sizeof(timeStr), "%H:%M", &timeinfo);

    // ---- Settings (load once) ----
    static String   lastQuoteTime  = "";
    static Settings settings;
    static bool     settingsLoaded = false;

    if (!settingsLoaded) {
        loadSettings(settings);
        settingsLoaded = true;
    }

    int hour = timeinfo.tm_hour;

    // ---- Night mode: freeze last quote 0:00 - 6:00 ----
    static bool nightDrawn = false;

    if (settings.nightMode && hour >= 0 && hour < 6) {
        if (!nightDrawn) {
            if (lastQuoteTime == "") {
                lastQuoteTime = String(timeStr);
            }
            auto quotes = loadQuotes(lastQuoteTime);
            if (!quotes.empty()) {
                showQuote(quotes[0], timeinfo);
            }
            nightDrawn = true;
        }
    } else {
        nightDrawn = false;

        // ---- Normal mode ----
        auto quotes = loadQuotes(String(timeStr));

        if (quotes.empty()) {
            M5.Display.clear();
            M5.Display.println("No quote for:");
            M5.Display.println(timeStr);
            M5.Display.display();
        } else {
            Quote q = quotes[random(quotes.size())];
            showQuote(q, timeinfo);
            lastQuoteTime = String(timeStr);
        }
    }

    // ---- Periodic NTP resync ----
    static unsigned long lastSync = 0;
    if (millis() - lastSync > NTP_RESYNC_INTERVAL_MS) {
        connectWiFi();
        syncTime();
        WiFi.mode(WIFI_OFF);
        lastSync = millis();
    }

    // ---- Wait ~60 s, but stay responsive to touch ----
    for (int i = 0; i < 600; i++) {
        delay(100);
        M5.update();

        auto td = M5.Touch.getDetail();
        if (td.isPressed()) {
            if (!touching) {
                touching   = true;
                touchStart = millis();
            }
            if (millis() - touchStart > LONG_PRESS_MS) {
                touching = false;
                startPortal();
            }
            break;  // exit wait so loop() refreshes immediately
        } else {
            touching = false;
        }
    }
}