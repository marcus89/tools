#!/usr/bin/env python3

from evdev import InputDevice, ecodes, categorize
import jack

# ==========================
# Configuration
# ==========================

KEYBOARD = "/dev/input/event4"

CHANNEL = 0          # MIDI channel (0 = channel 1)
VELOCITY = 100

# Keyboard -> MIDI Note
KEY_TO_NOTE = {
    ecodes.KEY_A: 60,   # C4
    ecodes.KEY_S: 62,   # D4
    ecodes.KEY_D: 64,   # E4
    ecodes.KEY_F: 65,   # F4
    ecodes.KEY_G: 67,   # G4
    ecodes.KEY_H: 69,   # A4
    ecodes.KEY_J: 71,   # B4
    ecodes.KEY_K: 72,   # C5
}

# Keyboard -> CC
KEY_TO_CC = {
    ecodes.KEY_Q: (1, 127),     # Mod wheel max
    ecodes.KEY_W: (1, 0),       # Mod wheel zero

    ecodes.KEY_E: (64, 127),    # Sustain ON
    ecodes.KEY_R: (64, 0),      # Sustain OFF
}

KEYS_TO_CC = {
    ecodes.KEY_T: [(1,127), (64,127)],
    ecodes.KEY_Y: [(1,0), (64,0)],
}

# ==========================
# JACK
# ==========================

client = jack.Client("keyboard2midi")
outport = client.midi_outports.register("out")

# Keep a list of messages to send each JACK cycle
pending_events = []

@client.set_process_callback
def process(frames):
    outport.clear_buffer()
    while pending_events:
        msg = pending_events.pop(0)
        outport.write_midi_event(0, msg)

client.activate()

print("JACK client started.")
print("Connect 'keyboard2midi:out' in QjackCtl.")

# ==========================
# Keyboard
# ==========================

dev = InputDevice(KEYBOARD)

print(f"Listening on {dev.name}")

try:
    for event in dev.read_loop():

        if event.type != ecodes.EV_KEY:
            continue

        key_event = categorize(event)
        key = key_event.scancode

        # Ignore auto-repeat
        if key_event.keystate == key_event.key_hold:
            continue

        #
        # NOTE
        #
        if key in KEY_TO_NOTE:

            note = KEY_TO_NOTE[key]

            if key_event.keystate == key_event.key_down:
                pending_events.append(bytes([
                    0x90 | CHANNEL,
                    note,
                    VELOCITY
                ]))

            elif key_event.keystate == key_event.key_up:
                pending_events.append(bytes([
                    0x80 | CHANNEL,
                    note,
                    0
                ]))

        #
        # CONTROL CHANGE
        #
        elif key in KEY_TO_CC:

            cc, value = KEY_TO_CC[key]

            if key_event.keystate == key_event.key_down:
                pending_events.append(bytes([
                    0xB0 | CHANNEL,
                    cc,
                    value
                ]))

        elif key in KEYS_TO_CC:
            if key_event.keystate == key_event.key_down:
                for (cc, value) in KEYS_TO_CC[key]:
                    print(f"{cc} {value}")
                    pending_events.append(bytes([
                        0xB0 | CHANNEL,
                        cc,
                        value
                    ]))

except KeyboardInterrupt:
    pass

finally:
    client.deactivate()
    client.close()