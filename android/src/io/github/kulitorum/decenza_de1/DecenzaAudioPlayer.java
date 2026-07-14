package io.github.kulitorum.decenza_de1;

import android.media.AudioAttributes;
import android.media.MediaPlayer;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

// [barista-fork] Native Android audio playback for the barista / coaching cloud-TTS mp3, the thinking-earcon
// loop, and the ElevenLabs voice-preview samples.
//
// WHY THIS EXISTS: on the DE1 tablet Qt's QMediaDevices only ever enumerates "Built in speaker" — it never
// sees a connected USB-C / Bluetooth speaker — so QMediaPlayer / QAudioOutput always play the barista out of
// the tablet even when the system media route is the external speaker. Android's OWN MediaPlayer, tagged with
// AudioAttributes.USAGE_MEDIA, DOES follow the system media route to the external speaker. So all barista
// audio plays through this instead of Qt.
//
// One instance per (C++ AssistantVoice, role): a `handle` (the AssistantVoice*) plus a `tag` identifying which
// player this is (0=voice, 1=thinking-cue, 2=preview). The static natives carry BOTH back so the C++ side can
// route the callback to the right player without touching the wrong state (e.g. the cue starting must never
// flip the barista's `speaking`). MediaPlayer must be created and driven from a thread with a Looper, and its
// async callbacks are delivered on that Looper; to keep every touch of `mp` on ONE thread we post all work to
// the main Looper (the same pattern DecenzaSpeech uses for SpeechRecognizer).
public class DecenzaAudioPlayer {
    private static final String TAG = "DecenzaAudioPlayer";
    private final Handler main = new Handler(Looper.getMainLooper());
    private final long handle;         // the C++ AssistantVoice* — routes callbacks back to the right instance
    private final int tag;             // which player: 0=voice, 1=cue(loop), 2=preview — carried into callbacks
    private MediaPlayer mp;            // touched only on the main Looper
    private int playId = 0;           // bumped by every play()/stop(); a callback whose myId != playId is stale
    private volatile boolean playing;  // best-effort state for isPlaying()

    public DecenzaAudioPlayer(long handle, int tag) {
        this.handle = handle;
        this.tag = tag;
    }

    // Play `path` (a local file path OR an http(s) URL — MediaPlayer.setDataSource accepts both) at `volume`
    // (0..1). Releases any current clip first. Follows the system media route. On prepared → nativeOnStarted;
    // on completion/error → nativeOnFinished. A play() immediately superseded by a newer play()/stop()
    // (barge-in) is suppressed via the playId generation guard.
    public void play(final String path, final float volume) {
        startInternal(path, volume, false);
    }

    // Like play(), but loops forever (the thinking earcon). onPrepared still fires nativeOnStarted; there is no
    // natural completion, so nativeOnFinished only comes on error — a stop() is the normal way it ends.
    public void playLooping(final String path, final float volume) {
        startInternal(path, volume, true);
    }

    private void startInternal(final String path, final float volume, final boolean loop) {
        main.post(new Runnable() {
            @Override public void run() {
                final int myId = ++playId;
                releaseLocked();
                try {
                    final MediaPlayer m = new MediaPlayer();
                    mp = m;
                    m.setAudioAttributes(new AudioAttributes.Builder()
                            .setUsage(AudioAttributes.USAGE_MEDIA)          // <-- follows the external-speaker route
                            .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
                            .build());
                    m.setLooping(loop);
                    m.setDataSource(path);
                    m.setVolume(volume, volume);
                    m.setOnPreparedListener(new MediaPlayer.OnPreparedListener() {
                        @Override public void onPrepared(MediaPlayer p) {
                            if (myId != playId) return;   // superseded by a newer play()/stop()
                            try { p.start(); } catch (Exception e) { Log.w(TAG, "start failed", e); }
                            playing = true;
                            // [barista-fork] Report the clip's real duration (ms) so the UI can time-sync the
                            // read-along text scroll to the actual speech. -1 for a looping earcon / unknown.
                            int durMs;
                            try { durMs = p.isLooping() ? -1 : p.getDuration(); } catch (Exception e) { durMs = -1; }
                            nativeOnStarted(handle, tag, durMs);
                        }
                    });
                    m.setOnCompletionListener(new MediaPlayer.OnCompletionListener() {
                        @Override public void onCompletion(MediaPlayer p) {
                            if (myId != playId) return;
                            if (loop) return;             // looping clips never "complete"
                            releaseLocked();
                            nativeOnFinished(handle, tag);
                        }
                    });
                    m.setOnErrorListener(new MediaPlayer.OnErrorListener() {
                        @Override public boolean onError(MediaPlayer p, int what, int extra) {
                            Log.w(TAG, "MediaPlayer error what=" + what + " extra=" + extra);
                            if (myId != playId) return true;
                            releaseLocked();
                            nativeOnFinished(handle, tag);   // release any C++ hold on error, don't wedge
                            return true;                     // handled — no further callbacks for this player
                        }
                    });
                    m.prepareAsync();
                } catch (Exception e) {
                    Log.w(TAG, "play failed for " + path, e);
                    if (myId == playId) {
                        releaseLocked();
                        nativeOnFinished(handle, tag);       // never leave the C++ side waiting on a dead play
                    }
                }
            }
        });
    }

    // Stop + release the current clip. Bumps playId so the current clip's pending callbacks are ignored — the
    // C++ side initiated the stop, so it already knows and does NOT need a nativeOnFinished.
    public void stop() {
        main.post(new Runnable() {
            @Override public void run() {
                playId++;
                releaseLocked();
            }
        });
    }

    public boolean isPlaying() {
        return playing;
    }

    // [barista-fork] Apply a new volume (0..1) to the CURRENTLY playing clip in real time, so a moved slider
    // takes effect immediately instead of only on the next utterance. NOTE on Bluetooth A2DP "absolute volume":
    // Android routes the app's per-track volume to some BT speakers as a no-op — the speaker's own volume is
    // then the real control. This still works for the built-in speaker and non-absolute-volume routes.
    public void setVolume(final float volume) {
        main.post(new Runnable() {
            @Override public void run() {
                if (mp != null) { try { mp.setVolume(volume, volume); } catch (Exception e) { /* ignore */ } }
            }
        });
    }

    // Must run on the main Looper. Tears down the current MediaPlayer if any; safe to call repeatedly.
    private void releaseLocked() {
        playing = false;
        if (mp != null) {
            try { mp.stop(); } catch (Exception e) { /* not started / already stopped — fine */ }
            try { mp.release(); } catch (Exception e) { /* ignore */ }
            mp = null;
        }
    }

    // Implemented in C++ (assistantvoice.cpp) and bound via QJniEnvironment::registerNativeMethods.
    private static native void nativeOnStarted(long handle, int tag, int durationMs);
    private static native void nativeOnFinished(long handle, int tag);
}
