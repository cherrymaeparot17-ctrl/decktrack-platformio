// ==========================================================
// DECKTRACK - TFT dashboard (ESP32-S3 + 2.8" ST7789, 320 x 240)
// ==========================================================
// Reads the same Supabase tables as the website (items, borrows,
// students) and draws the dashboard on the TFT:
//   - Total / Available / Borrowed / Overdue counts
//   - The 5 most recent transactions
//
// PlatformIO version. Libraries (TFT_eSPI, ArduinoJson) and the TFT pins
// are set in platformio.ini, so there is nothing to install by hand.
// WiFi name/password go in src/secrets.h.
//
// Press the BOOT button on the board to refresh right away.
// ==========================================================

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <time.h>

#include "secrets.h"   // WiFi name and password (kept out of GitHub)

// ---------- SETTINGS ----------
// WiFi name/password are in src/secrets.h
const char* WIFI_SSID = SECRET_WIFI_SSID;
const char* WIFI_PASS = SECRET_WIFI_PASS;

const char* SUPABASE_URL = "https://afnkvhshypzopwclerxy.supabase.co";
const char* SUPABASE_KEY = "sb_publishable_gj_OKhp5OuT96MqHWgMNqw_RP2L4QgW";

const unsigned long REFRESH_MS = 15000;   // auto refresh every 15 seconds
const long GMT_OFFSET_SEC = 8 * 3600;     // Philippines is UTC+8
const int BOOT_BUTTON = 0;                // BOOT button on most ESP32-S3 boards
// --------------------------------------------

// Screen size in landscape.
const int SCREEN_W = 320;
const int SCREEN_H = 240;

#define RGB(r, g, b) ((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))

const uint16_t COL_BG     = RGB(244, 247, 252);
const uint16_t COL_NAVY   = RGB(20, 33, 61);
const uint16_t COL_BLUE   = RGB(50, 103, 233);
const uint16_t COL_CARD   = RGB(255, 255, 255);
const uint16_t COL_BORDER = RGB(230, 234, 241);
const uint16_t COL_TEXT   = RGB(32, 43, 60);
const uint16_t COL_MUTED  = RGB(123, 135, 153);
const uint16_t COL_GREEN  = RGB(21, 155, 101);
const uint16_t COL_GREEN_BG  = RGB(229, 247, 238);
const uint16_t COL_ORANGE = RGB(185, 121, 21);
const uint16_t COL_ORANGE_BG = RGB(255, 243, 223);
const uint16_t COL_RED    = RGB(217, 68, 68);
const uint16_t COL_RED_BG = RGB(255, 235, 235);

TFT_eSPI tft = TFT_eSPI();

struct Stats {
    int total = 0;
    int available = 0;
    int borrowed = 0;
    int overdue = 0;
};

struct Row {
    String borrower;
    String equipment;
    String status;   // "Borrowed", "Returned" or "Overdue"
};

Stats stats;
Row rows[5];
int rowCount = 0;
bool online = false;
unsigned long lastRefresh = 0;

// ---------- helpers ----------

String todayISO() {
    struct tm t;
    if (!getLocalTime(&t, 1000)) return "";
    char buf[11];
    strftime(buf, sizeof(buf), "%Y-%m-%d", &t);
    return String(buf);
}

String fit(const String& s, int maxChars) {
    if ((int)s.length() <= maxChars) return s;
    String t = s.substring(0, maxChars - 1);
    t.trim();   // no stray space before the dot
    return t + ".";
}

void showMessage(const char* line1, const char* line2 = "") {
    tft.fillScreen(COL_BG);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(COL_TEXT, COL_BG);
    tft.drawString(line1, SCREEN_W / 2, 105, 4);
    tft.setTextColor(COL_MUTED, COL_BG);
    tft.drawString(line2, SCREEN_W / 2, 140, 2);
}

// ---------- Supabase ----------

// Sends GET /rest/v1/<path> and parses the JSON answer into doc.
bool supabaseGet(const String& path, JsonDocument& doc) {
    if (WiFi.status() != WL_CONNECTED) return false;

    WiFiClientSecure client;
    client.setInsecure();   // skips the certificate check (fine for a prototype)

    HTTPClient http;
    http.setTimeout(10000);

    String url = String(SUPABASE_URL) + "/rest/v1/" + path;
    if (!http.begin(client, url)) return false;

    http.addHeader("apikey", SUPABASE_KEY);
    http.addHeader("Accept", "application/json");

    int code = http.GET();
    if (code != 200) {
        Serial.printf("GET %s -> HTTP %d\n", path.c_str(), code);
        if (code > 0) Serial.println(http.getString());
        http.end();
        return false;
    }

    String body = http.getString();
    http.end();

    DeserializationError err = deserializeJson(doc, body.c_str());
    if (err) {
        Serial.printf("JSON error: %s\n", err.c_str());
        return false;
    }
    return true;
}

bool loadStats(const String& today) {
    JsonDocument items;
    if (!supabaseGet("items?select=quantity,available", items)) return false;

    Stats s;
    for (JsonObject it : items.as<JsonArray>()) {
        s.total += it["quantity"] | 0;
        s.available += it["available"] | 0;
    }

    JsonDocument openBorrows;
    if (!supabaseGet("borrows?returned_at=is.null&select=quantity,due_date", openBorrows)) {
        return false;
    }

    for (JsonObject b : openBorrows.as<JsonArray>()) {
        int q = b["quantity"] | 1;
        s.borrowed += q;

        const char* due = b["due_date"] | "";
        if (today.length() && due[0] && strcmp(due, today.c_str()) < 0) {
            s.overdue += q;
        }
    }

    stats = s;
    return true;
}

bool loadRecent(const String& today) {
    JsonDocument doc;
    if (!supabaseGet(
            "borrows?select=quantity,returned_at,due_date,students(name),items(name)"
            "&order=id.desc&limit=5",
            doc)) {
        return false;
    }

    rowCount = 0;
    for (JsonObject b : doc.as<JsonArray>()) {
        if (rowCount >= 5) break;

        const char* who = b["students"]["name"] | "Unknown";
        const char* what = b["items"]["name"] | "Unknown";

        String status = "Borrowed";
        if (!b["returned_at"].isNull()) {
            status = "Returned";
        } else {
            const char* due = b["due_date"] | "";
            if (today.length() && due[0] && strcmp(due, today.c_str()) < 0) {
                status = "Overdue";
            }
        }

        rows[rowCount].borrower = String(who);
        rows[rowCount].equipment = String(what);
        rows[rowCount].status = status;
        rowCount++;
    }
    return true;
}

// ---------- drawing (320 x 240) ----------

void drawHeader() {
    tft.fillRect(0, 0, SCREEN_W, 34, COL_NAVY);

    tft.fillRoundRect(6, 4, 26, 26, 6, COL_BLUE);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, COL_BLUE);
    tft.drawString("D", 19, 17, 4);

    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_WHITE, COL_NAVY);
    tft.drawString("DECKTRACK", 40, 4, 4);

    uint16_t dot = online ? COL_GREEN : COL_RED;
    tft.fillCircle(254, 17, 4, dot);
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(TFT_WHITE, COL_NAVY);
    tft.drawString(online ? "Online" : "Offline", 314, 9, 2);
}

// Four tiles across: each 75 wide with a 4 px gap, starting at x = 4.
void drawTile(int index, const char* label, int value, uint16_t color) {
    int x = 4 + index * 79;
    int y = 38;

    tft.fillRoundRect(x, y, 75, 56, 6, COL_CARD);
    tft.drawRoundRect(x, y, 75, 56, 6, COL_BORDER);

    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(COL_MUTED, COL_CARD);
    tft.drawString(label, x + 8, y + 8, 1);

    tft.setTextColor(color, COL_CARD);
    tft.drawString(String(value), x + 8, y + 24, 4);
}

void drawRow(int index, const Row& r) {
    int y = 122 + index * 22;

    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(COL_TEXT, COL_CARD);
    tft.drawString(fit(r.borrower, 11), 12, y + 3, 2);

    tft.setTextColor(COL_MUTED, COL_CARD);
    tft.drawString(fit(r.equipment, 13), 112, y + 3, 2);

    uint16_t fg = COL_ORANGE;
    uint16_t bg = COL_ORANGE_BG;
    if (r.status == "Returned") {
        fg = COL_GREEN;
        bg = COL_GREEN_BG;
    } else if (r.status == "Overdue") {
        fg = COL_RED;
        bg = COL_RED_BG;
    }

    tft.fillRoundRect(236, y + 3, 74, 16, 8, bg);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(fg, bg);
    tft.drawString(r.status, 273, y + 11, 1);

    tft.drawFastHLine(10, y + 21, 300, COL_BORDER);
}

void drawDashboard() {
    tft.fillScreen(COL_BG);
    drawHeader();

    drawTile(0, "Total",     stats.total,     COL_BLUE);
    drawTile(1, "Available", stats.available, COL_GREEN);
    drawTile(2, "Borrowed",  stats.borrowed,  COL_ORANGE);
    drawTile(3, "Overdue",   stats.overdue,   COL_RED);

    tft.fillRoundRect(4, 98, 312, 138, 6, COL_CARD);
    tft.drawRoundRect(4, 98, 312, 138, 6, COL_BORDER);

    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(COL_TEXT, COL_CARD);
    tft.drawString("Recent Transactions", 12, 103, 2);

    if (rowCount == 0) {
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(COL_MUTED, COL_CARD);
        tft.drawString(online ? "No transactions yet." : "No data yet.", SCREEN_W / 2, 175, 2);
        return;
    }

    for (int i = 0; i < rowCount; i++) {
        drawRow(i, rows[i]);
    }
}

// ---------- main flow ----------

void refresh() {
    lastRefresh = millis();

    if (WiFi.status() != WL_CONNECTED) {
        WiFi.reconnect();
        delay(1500);
    }

    String today = todayISO();
    bool ok = loadStats(today) && loadRecent(today);

    online = ok;
    drawDashboard();   // keeps the last good numbers if the refresh failed
}

void setup() {
    Serial.begin(115200);
    pinMode(BOOT_BUTTON, INPUT_PULLUP);

    tft.init();
    tft.setRotation(1);   // landscape 320 x 240. Try 3 if the picture is upside down.

    showMessage("DECKTRACK", "Connecting to WiFi...");

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
        delay(300);
    }

    if (WiFi.status() != WL_CONNECTED) {
        showMessage("WiFi failed", "Check name/password, press RESET");
        delay(2000);
        // Keep going: refresh() will try to reconnect.
    } else {
        Serial.print("WiFi connected, IP: ");
        Serial.println(WiFi.localIP());

        configTime(GMT_OFFSET_SEC, 0, "pool.ntp.org", "time.google.com");
        showMessage("Connected", "Loading data...");
    }

    refresh();
}

void loop() {
    if (millis() - lastRefresh >= REFRESH_MS) {
        refresh();
    }

    if (digitalRead(BOOT_BUTTON) == LOW) {
        refresh();
        delay(400);   // simple debounce
    }

    delay(20);
}
