#pragma once

#include <stddef.h>
#include <stdint.h>

// A Bluetooth push button used as a remote while rowing: the Shelly BLU Button (Tough) 1.
// The button never connects. It broadcasts BTHome v2 packets (service data 0xFCD2) with a button
// event, so this only listens to advertisements. The scan itself runs in the hr task, which owns the
// Bluetooth stack; this module parses the packets, remembers the paired button and queues the events.
// Encrypted BTHome is not supported: leave the button in its default (unencrypted) mode.
namespace remote {

enum class Event : uint8_t { None, Single, Double, Triple, Long, DoubleLong, TripleLong };

void begin();

// Settings side: the next button press seen in range is registered. Safe to call from any task.
void startPairing();
void forget();
bool hasSaved();
bool pairing();
const char* name(char* out, size_t n);  // saved button, empty if none

// UI side: oldest queued event, false when there is none.
bool popEvent(Event& e);
const char* eventName(Event e);

// Test hook (serial commands, simulator): queue an event as if the button sent it.
void inject(Event e);

// Bluetooth task side.
void onAdvert(const uint8_t* addr, uint8_t addrType, int rssi, const uint8_t* payload, size_t len);
bool takePairRequest();    // true once per startPairing()
void pairBegin();          // pairing scan starts
bool pairFound();          // a button was heard pressing
void pairEnd();            // pairing scan finished: saves the button if one was heard

}  // namespace remote
