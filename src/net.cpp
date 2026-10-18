#include "net.h"

#include <DNSServer.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>

namespace net {
namespace {

constexpr uint32_t CONNECT_TIMEOUT_MS = 20000;
constexpr uint32_t SCAN_INTERVAL_MS = 15000;

State st = State::Off;
uint32_t connectStartMs = 0;
String pendingSsid, pendingPass, currentSsid;

WebServer* server = nullptr;
DNSServer* dns = nullptr;
char apName[24] = "";
uint32_t lastScanMs = 0;
uint32_t connectedInSetupMs = 0;
bool connectFromPage = false;  // only a network chosen on the setup page closes the setup network

String htmlEscape(const String& s) {
    String o;
    for (size_t i = 0; i < s.length(); i++) {
        char c = s[i];
        if (c == '<') o += "&lt;";
        else if (c == '>') o += "&gt;";
        else if (c == '&') o += "&amp;";
        else if (c == '"') o += "&quot;";
        else o += c;
    }
    return o;
}

const char* PAGE_HEAD =
    "<!DOCTYPE html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Tab5 rowing Wi-Fi</title><style>body{font-family:sans-serif;margin:24px;max-width:420px}"
    "input,button{font-size:18px;width:100%;padding:10px;margin:6px 0 14px;box-sizing:border-box}"
    "button{background:#1565c0;color:#fff;border:0;border-radius:6px}</style></head><body>";

void handleRoot() {
    String page = PAGE_HEAD;
    page += "<h2>Tab5 rowing monitor</h2><p>Choose the Wi-Fi network the Tab5 should use to download maps "
            "(2.4 GHz only).</p><form method=post action=/save><label>Network</label>"
            "<input name=s list=nets autocomplete=off required><datalist id=nets>";
    int n = WiFi.scanComplete();
    for (int i = 0; i < n; i++) {
        String s = WiFi.SSID(i);
        if (s.length()) page += "<option value=\"" + htmlEscape(s) + "\">";
    }
    page += "</datalist><label>Password</label><input name=p type=password autocomplete=off>"
            "<button>Connect</button></form>";
    if (n > 0) {
        page += "<p>Networks in range:<br>";
        for (int i = 0; i < n && i < 15; i++) page += htmlEscape(WiFi.SSID(i)) + " (" + WiFi.RSSI(i) + " dBm)<br>";
        page += "</p>";
    }
    page += "</body></html>";
    server->send(200, "text/html", page);
}

void handleSave() {
    String s = server->arg("s"), p = server->arg("p");
    s.trim();
    if (s.isEmpty()) {
        server->sendHeader("Location", "/");
        server->send(302);
        return;
    }
    String page = PAGE_HEAD;
    page += "<h2>Connecting...</h2><p>The Tab5 is joining <b>" + htmlEscape(s) +
            "</b>. Watch its screen: the top bar shows WIFI when it worked. This setup network then "
            "closes; if the password was wrong, it stays open so you can try again.</p></body></html>";
    server->send(200, "text/html", page);
    connect(s.c_str(), p.c_str(), Serial);
    connectFromPage = true;
}

// Phones probe a fixed URL to detect a captive portal; send everything to the form.
void handleOther() {
    server->sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/");
    server->send(302);
}

}  // namespace

bool connect(const char* ssid, const char* pass, Print& out) {
    String s = ssid ? ssid : "", p = pass ? pass : "";
    if (s.isEmpty()) {
        Preferences prefs;
        prefs.begin("net", true);
        s = prefs.getString("ssid", "");
        p = prefs.getString("pass", "");
        prefs.end();
        if (s.isEmpty()) {
            out.println("[wifi] ERR no stored network; send W <ssid> <password>");
            st = State::Failed;
            return false;
        }
    }

    out.printf("[wifi] connecting to '%s' (password %u chars)...\n", s.c_str(), (unsigned)p.length());
    connectFromPage = false;
    pendingSsid = s;
    pendingPass = p;
    currentSsid = s;
    WiFi.mode(server ? WIFI_AP_STA : WIFI_STA);
    WiFi.begin(s.c_str(), p.c_str());
    connectStartMs = millis();
    st = State::Connecting;
    return true;
}

void loop(Print& out) {
    if (st == State::Connecting) {
        if (WiFi.status() == WL_CONNECTED) {
            out.printf("[wifi] OK %s rssi %d dBm in %lu ms\n", WiFi.localIP().toString().c_str(), WiFi.RSSI(),
                       (unsigned long)(millis() - connectStartMs));
            Preferences prefs;
            prefs.begin("net", false);
            prefs.putString("ssid", pendingSsid);
            prefs.putString("pass", pendingPass);
            prefs.end();
            st = State::Connected;
            if (server && connectFromPage) connectedInSetupMs = millis();
            connectFromPage = false;
        } else if (millis() - connectStartMs > CONNECT_TIMEOUT_MS) {
            // 1 = network not found (or 5 GHz only), 4 = connect failed (usually wrong password).
            out.printf("[wifi] ERR not connected after %lu ms (status %d)\n",
                       (unsigned long)(millis() - connectStartMs), (int)WiFi.status());
            WiFi.disconnect(false);
            if (!server) WiFi.mode(WIFI_OFF);
            st = State::Failed;
        }
    } else if (st == State::Connected && WiFi.status() != WL_CONNECTED) {
        out.println("[wifi] connection lost");
        st = State::Failed;
    }

    if (server) {
        dns->processNextRequest();
        server->handleClient();
        if (WiFi.scanComplete() != WIFI_SCAN_RUNNING && millis() - lastScanMs > SCAN_INTERVAL_MS &&
            st != State::Connecting) {
            lastScanMs = millis();
            WiFi.scanNetworks(true);
        }
        // Give the phone a moment to load the reply before the setup network closes.
        if (connectedInSetupMs && millis() - connectedInSetupMs > 5000) stopSetup();
    }
}

void disconnect() {
    if (server) stopSetup();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    st = State::Off;
}

bool hasStoredNetwork() {
    Preferences prefs;
    prefs.begin("net", true);
    bool has = prefs.isKey("ssid") && prefs.getString("ssid", "").length() > 0;
    prefs.end();
    return has;
}

State state() { return st; }

bool connected() { return st == State::Connected && WiFi.status() == WL_CONNECTED; }

const char* ssid() {
    if (currentSsid.isEmpty()) {
        Preferences prefs;
        prefs.begin("net", true);
        currentSsid = prefs.getString("ssid", "");
        prefs.end();
    }
    return currentSsid.c_str();
}

void startSetup(Print& out) {
    if (server) return;
    snprintf(apName, sizeof(apName), "TabRowing-%04X", (unsigned)(ESP.getEfuseMac() & 0xFFFF));
    WiFi.mode(WIFI_AP_STA);
    if (!WiFi.softAP(apName)) {
        out.println("[wifi] ERR could not start the setup network");
        WiFi.mode(st == State::Off || st == State::Failed ? WIFI_OFF : WIFI_STA);
        return;
    }
    dns = new DNSServer();
    dns->start(53, "*", WiFi.softAPIP());
    server = new WebServer(80);
    server->on("/", HTTP_GET, handleRoot);
    server->on("/save", HTTP_POST, handleSave);
    server->onNotFound(handleOther);
    server->begin();
    connectedInSetupMs = 0;
    lastScanMs = millis();
    WiFi.scanNetworks(true);
    out.printf("[wifi] setup network '%s' open, page at http://%s/\n", apName,
               WiFi.softAPIP().toString().c_str());
}

void stopSetup() {
    if (!server) return;
    server->stop();
    delete server;
    server = nullptr;
    dns->stop();
    delete dns;
    dns = nullptr;
    WiFi.scanDelete();
    WiFi.softAPdisconnect(true);
    WiFi.mode(st == State::Connected || st == State::Connecting ? WIFI_STA : WIFI_OFF);
    connectedInSetupMs = 0;
    Serial.println("[wifi] setup network closed");
}

bool setupActive() { return server != nullptr; }

bool setupClosing() { return server && connectedInSetupMs; }

const char* setupName() { return apName; }

}  // namespace net
