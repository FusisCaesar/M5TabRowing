#pragma once

#include <Print.h>

// Wi-Fi through the Tab5's ESP32-C6 co-processor (ESP-Hosted over SDIO), used to
// download maps. Credentials are kept in NVS. The radio stays off until asked.
namespace net {

enum class State : uint8_t { Off, Connecting, Connected, Failed };

// Starts connecting to `ssid` in the background; the credentials are stored once
// it succeeds. With an empty ssid, uses the stored credentials.
bool connect(const char* ssid, const char* pass, Print& out);
void disconnect();
bool hasStoredNetwork();

// Call from the main loop: reports the connect result, handles timeouts and
// serves the setup page.
void loop(Print& out);
State state();
bool connected();
const char* ssid();  // network being used or last used

// Setup page: the Tab5 opens its own open network (setupName()) and serves a page
// at http://192.168.4.1 where a phone picks the home network and enters the
// password. Stops by itself once the Tab5 is connected.
void startSetup(Print& out);
void stopSetup();
bool setupActive();
bool setupClosing();  // connected from the setup page; the setup network closes in a few seconds
const char* setupName();

}  // namespace net
