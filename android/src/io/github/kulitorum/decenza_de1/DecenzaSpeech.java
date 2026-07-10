package io.github.kulitorum.decenza_de1;

import android.content.Context;
import android.content.Intent;
import android.media.AudioManager;
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

    // Implemented in C++ and bound via QJniEnvironment::registerNativeMethods.
    public static native void nativeOnFinal(String text);
    public static native void nativeOnPartial(String text);
    public static native void nativeOnError(int code);

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

    public static void start(final Context ctx, final boolean preferOffline) {
        main.post(new Runnable() {
            @Override public void run() {
                try {
                    // Heal any stream left muted by an earlier build; no muting is done anymore.
                    recoverMutedStreams(ctx);
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
        @Override public void onError(int error) { nativeOnError(error); }
        @Override public void onReadyForSpeech(Bundle params) {}
        @Override public void onBeginningOfSpeech() {}
        @Override public void onRmsChanged(float rmsdB) {}
        @Override public void onBufferReceived(byte[] buffer) {}
        @Override public void onEndOfSpeech() {}
        @Override public void onEvent(int eventType, Bundle params) {}
    };
}
