package io.github.kulitorum.decenza_de1;

import android.content.Context;
import android.content.Intent;
import android.media.AudioDeviceInfo;
import android.media.AudioManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.speech.RecognitionListener;
import android.speech.RecognizerIntent;
import android.speech.SpeechRecognizer;

import java.util.ArrayList;

// [barista-fork] On-device speech recognition for the barista assistant. SpeechRecognizer must be
// created and driven on the Android main thread, so every entry point posts to the main Looper.
// Results are handed back to C++ (VoiceInput) via the registered native methods below.
public class DecenzaSpeech {
    private static SpeechRecognizer recognizer;
    private static final Handler main = new Handler(Looper.getMainLooper());
    // [barista-fork] After a MALFUNCTION error (ERROR_CLIENT=5, ERROR_RECOGNIZER_BUSY=8) the platform
    // SpeechRecognizer instance is often left wedged — calling startListening() on it again just re-fires
    // ERROR_CLIENT and the user's next utterance is silently dropped (the "says listening, never hears me,
    // works if I repeat" loop, confirmed in the stt diagnostics as repeated code=5 right after mic resume).
    // The reliable cure is to DESTROY and recreate the recognizer before the next start. We do NOT do this for
    // the benign silence family (timeout=6 / no-match=7), which fire normally across a conversational pause —
    // recreating on those would churn the recogniser through idle listening for no benefit.
    private static volatile boolean recreateOnNextStart = false;

    // Implemented in C++ and bound via QJniEnvironment::registerNativeMethods.
    public static native void nativeOnFinal(String text);
    public static native void nativeOnPartial(String text);
    public static native void nativeOnError(int code);
    // [barista-fork] One-line device-audio-route snapshot → the barista diagnostics log (stt/mic_route),
    // so a Bluetooth "lost first words" report can be confirmed and the built-in-mic override verified.
    public static native void nativeMicDiag(String info);

    // [barista-fork] We used to mute STREAM_MUSIC/NOTIFICATION/SYSTEM around each listen to hide the
    // SpeechRecognizer's start/stop earcon. That was removed: muting media/system streams was too broad
    // (STREAM_MUSIC also carries the barista's TTS and all app audio), and a listen that ended without a
    // terminal callback (recognizer death, app backgrounded mid-listen) left the streams stuck muted —
    // i.e. "sounds are off" on the tablet. The recognizer's earcon is preferable to that risk.
    //
    // One-time recovery: a device that ran an earlier (muting) build may still have those streams muted,
    // so on the first listen we UNMUTE them once to heal that state, then never touch audio streams again.
    private static AudioManager audio;
    private static boolean streamsRecovered = false;
    private static final int[] RECOVER_STREAMS = {
        AudioManager.STREAM_MUSIC,
        AudioManager.STREAM_NOTIFICATION,
        AudioManager.STREAM_SYSTEM,
    };

    private static void recoverMutedStreams(Context ctx) {
        if (streamsRecovered) return;
        streamsRecovered = true;   // set first: a failure here must not retry-loop on every listen
        try {
            if (audio == null)
                audio = (AudioManager) ctx.getApplicationContext().getSystemService(Context.AUDIO_SERVICE);
            if (audio == null) return;
            for (int s : RECOVER_STREAMS)
                audio.adjustStreamVolume(s, AudioManager.ADJUST_UNMUTE, 0);
        } catch (Exception ignored) {}
    }

    // [barista-fork] Bluetooth "loses the first few seconds" fix. When a Bluetooth headset is connected,
    // Android routes the recogniser's mic to the Bluetooth SCO link; bringing SCO up AFTER our A2DP TTS
    // eats the user's opening ~1-2s. We keep TTS on A2DP (USAGE_MEDIA, full quality) and force the STT
    // capture onto the BUILT-IN mic via setCommunicationDevice(TYPE_BUILTIN_MIC). Whether the system
    // recogniser honours this is device/version-specific, so we LOG the route + whether the override took
    // (stt/mic_route) — the next session's log tells us. API 31+ only; no-op otherwise. Never throws into
    // the listen path.
    private static void preferBuiltInMicForBluetooth(Context ctx) {
        try {
            if (audio == null)
                audio = (AudioManager) ctx.getApplicationContext().getSystemService(Context.AUDIO_SERVICE);
            if (audio == null) return;
            StringBuilder sb = new StringBuilder();
            sb.append("sco=").append(audio.isBluetoothScoOn());
            if (Build.VERSION.SDK_INT < 31) { nativeMicDiag(sb.append(" api<31").toString()); return; }

            AudioDeviceInfo builtin = null;
            boolean btInput = false;
            sb.append(" inputs=");
            for (AudioDeviceInfo d : audio.getDevices(AudioManager.GET_DEVICES_INPUTS)) {
                sb.append(d.getType()).append(",");
                if (d.getType() == AudioDeviceInfo.TYPE_BUILTIN_MIC) builtin = d;
                if (d.getType() == AudioDeviceInfo.TYPE_BLUETOOTH_SCO) btInput = true;
            }
            AudioDeviceInfo commDev = audio.getCommunicationDevice();
            sb.append(" commDev=").append(commDev != null ? commDev.getType() : -1);
            // Output route — so the log can confirm the barista's TTS (USAGE_MEDIA) STAYS on Bluetooth A2DP
            // and the input-mic override didn't drag it onto the built-in speaker (the owner's key UX risk).
            sb.append(" a2dp=").append(audio.isBluetoothA2dpOn());
            sb.append(" outs=");
            for (AudioDeviceInfo d : audio.getDevices(AudioManager.GET_DEVICES_OUTPUTS))
                sb.append(d.getType()).append(",");
            // setCommunicationDevice only accepts devices from getAvailableCommunicationDevices() — an
            // OUTPUT-anchored list that usually EXCLUDES TYPE_BUILTIN_MIC, so this likely returns false and
            // changes nothing (forceBuiltin=false in the log). It's a harmless best-effort try: if a given
            // Samsung build DOES accept the built-in mic it fixes the route; if not, the log tells us to move
            // to the own-AudioRecord (EXTRA_AUDIO_SOURCE) approach. Only attempted when a BT mic is present.
            if (btInput && builtin != null) {
                boolean ok = audio.setCommunicationDevice(builtin);
                sb.append(" forceBuiltin=").append(ok);
            } else {
                sb.append(" forceBuiltin=skip");
            }
            nativeMicDiag(sb.toString());
        } catch (Throwable t) {
            try { nativeMicDiag("mic_route_error " + t.getClass().getSimpleName()); } catch (Throwable ignored) {}
        }
    }

    public static void start(final Context ctx, final boolean preferOffline) {
        main.post(new Runnable() {
            @Override public void run() {
                try {
                    // Heal any stream left muted by an earlier build; no muting is done anymore.
                    recoverMutedStreams(ctx);
                    // Keep the mic off Bluetooth SCO (built-in mic) so the first words aren't lost.
                    preferBuiltInMicForBluetooth(ctx);
                    // [barista-fork] A prior malfunction (code 5/8) leaves the recogniser wedged — tear it down so
                    // the block below builds a fresh one, instead of re-starting a broken instance into ERROR_CLIENT.
                    if (recreateOnNextStart && recognizer != null) {
                        try { recognizer.destroy(); } catch (Exception ignored) {}
                        recognizer = null;
                    }
                    recreateOnNextStart = false;
                    if (recognizer == null) {
                        recognizer = SpeechRecognizer.createSpeechRecognizer(ctx);
                        recognizer.setRecognitionListener(listener);
                    }
                    Intent intent = new Intent(RecognizerIntent.ACTION_RECOGNIZE_SPEECH);
                    intent.putExtra(RecognizerIntent.EXTRA_LANGUAGE_MODEL,
                                    RecognizerIntent.LANGUAGE_MODEL_FREE_FORM);
                    intent.putExtra(RecognizerIntent.EXTRA_PARTIAL_RESULTS, true);
                    // On-device by default (audio stays on the tablet); C++ retries with this off if
                    // the offline model is unavailable (the user's "pragmatic" privacy choice).
                    if (preferOffline)
                        intent.putExtra(RecognizerIntent.EXTRA_PREFER_OFFLINE, true);
                    intent.putExtra(RecognizerIntent.EXTRA_CALLING_PACKAGE, ctx.getPackageName());
                    recognizer.startListening(intent);
                } catch (Exception e) {
                    nativeOnError(-1);
                }
            }
        });
    }

    public static void stop() {
        main.post(new Runnable() {
            @Override public void run() {
                if (recognizer != null) {
                    try { recognizer.cancel(); } catch (Exception ignored) {}
                }
                // Release the built-in-mic override so normal (non-barista) audio routing resumes.
                try {
                    if (audio != null && Build.VERSION.SDK_INT >= 31)
                        audio.clearCommunicationDevice();
                } catch (Exception ignored) {}
            }
        });
    }

    private static final RecognitionListener listener = new RecognitionListener() {
        @Override public void onResults(Bundle results) {
            ArrayList<String> list = results.getStringArrayList(SpeechRecognizer.RESULTS_RECOGNITION);
            nativeOnFinal(list != null && !list.isEmpty() ? list.get(0) : "");
        }
        @Override public void onPartialResults(Bundle partial) {
            ArrayList<String> list = partial.getStringArrayList(SpeechRecognizer.RESULTS_RECOGNITION);
            if (list != null && !list.isEmpty()) nativeOnPartial(list.get(0));
        }
        @Override public void onError(int error) {
            // [barista-fork] Malfunction family (5=client, 8=busy) wedges the recogniser — force a fresh
            // instance on the next start so the restart the C++ side triggers doesn't re-fire ERROR_CLIENT and
            // eat the user's next utterance. Benign silence (6/7) leaves the recogniser fine, so no recreate.
            if (error == SpeechRecognizer.ERROR_CLIENT || error == SpeechRecognizer.ERROR_RECOGNIZER_BUSY)
                recreateOnNextStart = true;
            nativeOnError(error);
        }
        @Override public void onReadyForSpeech(Bundle params) {}
        @Override public void onBeginningOfSpeech() {}
        @Override public void onRmsChanged(float rmsdB) {}
        @Override public void onBufferReceived(byte[] buffer) {}
        @Override public void onEndOfSpeech() {}
        @Override public void onEvent(int eventType, Bundle params) {}
    };
}
