package io.github.kulitorum.decenza_de1;

import android.media.AudioDeviceInfo;
import android.media.AudioManager;

import java.util.HashSet;

// [barista-fork] Shared audio-device identity + enumeration for the barista Microphone/Speaker pickers.
// ONE place builds the stable key and friendly label and resolves a saved key back to a live device, so the
// input path (DecenzaSpeech) and the output path (DecenzaAudioPlayer) agree on the format and a saved choice
// survives reconnect/reboot. The key is TYPE + product name, deliberately NOT AudioDeviceInfo.getId() — ids
// are reassigned across disconnect/reconnect, so an id-keyed selection silently points at the wrong or a dead
// device later. A key that no longer resolves means the device is gone → the caller falls back to its default.
public final class DecenzaAudioDevices {
    private DecenzaAudioDevices() {}

    // Separators for the enumerated list handed to C++ (never occur in a product name or a "type:name" key):
    // TAB between a key and its label, NEWLINE between entries.
    private static final char KV = '\t';
    private static final char SEP = '\n';

    public static String key(AudioDeviceInfo d) {
        return d.getType() + ":" + safeName(d);
    }

    public static String label(AudioDeviceInfo d) {
        final String name = safeName(d);
        switch (d.getType()) {
            case AudioDeviceInfo.TYPE_BUILTIN_MIC:      return "Tablet microphone";
            case AudioDeviceInfo.TYPE_BUILTIN_SPEAKER:  return "Tablet speaker";
            case AudioDeviceInfo.TYPE_USB_HEADSET:
            case AudioDeviceInfo.TYPE_USB_DEVICE:
            case AudioDeviceInfo.TYPE_USB_ACCESSORY:    return (name.isEmpty() ? "USB audio" : name) + " (USB)";
            case AudioDeviceInfo.TYPE_BLUETOOTH_A2DP:
            case AudioDeviceInfo.TYPE_BLUETOOTH_SCO:    return (name.isEmpty() ? "Bluetooth" : name) + " (Bluetooth)";
            case AudioDeviceInfo.TYPE_WIRED_HEADSET:
            case AudioDeviceInfo.TYPE_WIRED_HEADPHONES: return (name.isEmpty() ? "Wired" : name) + " (wired)";
            default:                                    return name.isEmpty() ? ("Audio device " + d.getType()) : name;
        }
    }

    // Enumerate one direction (GET_DEVICES_INPUTS / GET_DEVICES_OUTPUTS) as "key<US>label<RS>key<US>label…".
    // De-dupes by key: a device can enumerate more than once (e.g. two built-in mics, front + back).
    public static String list(AudioManager audio, int flag) {
        final StringBuilder sb = new StringBuilder();
        final HashSet<String> seen = new HashSet<>();
        for (AudioDeviceInfo d : audio.getDevices(flag)) {
            final String k = key(d);
            if (!seen.add(k)) continue;
            if (sb.length() > 0) sb.append(SEP);
            sb.append(k).append(KV).append(label(d));
        }
        return sb.toString();
    }

    // Resolve a saved key to a live device of the given direction, or null if it is not currently present
    // (unplugged / not yet reconnected) — the caller then falls back to its default rather than going deaf.
    public static AudioDeviceInfo resolve(AudioManager audio, int flag, String savedKey) {
        if (savedKey == null || savedKey.isEmpty()) return null;
        for (AudioDeviceInfo d : audio.getDevices(flag))
            if (key(d).equals(savedKey)) return d;
        return null;
    }

    private static String safeName(AudioDeviceInfo d) {
        final CharSequence n = d.getProductName();
        return n == null ? "" : n.toString().trim();
    }
}
