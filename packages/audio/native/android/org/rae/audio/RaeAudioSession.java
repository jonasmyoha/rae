// packages/audio's Android session (docs/audio-design.md §9): the audio
// focus, the "becoming noisy" broadcast and a MediaSession, for
// audio/AudioSession and audio/NowPlaying. native/rae_audio_glue.c is the other
// half: it calls these static methods through JNI, one call each, and the
// events come back through the two native methods into the same queues the
// iOS observers fill. Which focus an intent asks for, and what a button does,
// is Rae.
//
// Platform reason: the audio focus, the noisy broadcast and MediaSession are
// Java-only Android APIs.
package org.rae.audio;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.media.AudioAttributes;
import android.media.AudioFocusRequest;
import android.media.AudioManager;
import android.media.MediaMetadata;
import android.media.session.MediaSession;
import android.media.session.PlaybackState;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.os.Process;

import org.libsdl.app.SDLActivity;

public final class RaeAudioSession {
  // The event kinds, as rae_audio_session_inject numbers them (audio/AudioSession)
  private static final int INTERRUPTION_BEGAN = 1;
  private static final int INTERRUPTION_ENDED = 2;
  private static final int ROUTE_LOST = 3;
  private static final int DUCKED = 8;
  // The commands, as rae_audio_remote_inject numbers them (audio/NowPlaying)
  private static final int PLAY = 1;
  private static final int PAUSE = 2;
  private static final int NEXT = 4;
  private static final int PREVIOUS = 5;
  private static final int CHANGE_POSITION = 6;

  private static native void nativeEvent(int kind, int value);

  private static native void nativeRemote(int kind, long value);

  private static final Handler MAIN = new Handler(Looper.getMainLooper());

  // The focus listener. Android tells focus holders apart by a text made of
  // the AudioManager's and the listener's default toString (MediaFocusControl
  // matches an abandon by that text alone), and two apps forked alike from the
  // zygote can get the same identity hashes: one Rae app giving its focus back
  // then removed another's (seen on the emulator). This text is the app's own.
  private static final class FocusListener implements AudioManager.OnAudioFocusChangeListener {
    private final String name;

    FocusListener(String packageName) {
      name = "rae-focus-" + packageName + "-" + Process.myPid();
    }

    @Override
    public void onAudioFocusChange(int change) {
      focusChanged(change);
    }

    @Override
    public String toString() {
      return name;
    }
  }

  private static FocusListener listener;
  private static AudioFocusRequest focus;
  private static boolean observing;
  private static MediaSession session;
  private static long actions;
  private static String shown = "";

  private static Context context() {
    return SDLActivity.getContext();
  }

  private static AudioManager audioManager() {
    Context context = context();
    return context == null ? null : (AudioManager) context.getSystemService(Context.AUDIO_SERVICE);
  }

  // Ask for the audio focus: `gain` an AudioManager.AUDIOFOCUS_GAIN* value,
  // `usage` and `content` AudioAttributes values. 1 granted, 2 delayed, 0 refused.
  public static int requestFocus(int gain, int usage, int content) {
    AudioManager manager = audioManager();
    if (manager == null || Build.VERSION.SDK_INT < 26) {
      return 0;
    }
    if (focus != null) {
      manager.abandonAudioFocusRequest(focus);
    }
    if (listener == null) {
      listener = new FocusListener(context().getPackageName());
    }
    AudioAttributes attributes = new AudioAttributes.Builder().setUsage(usage).setContentType(content).build();
    focus = new AudioFocusRequest.Builder(gain)
        .setAudioAttributes(attributes)
        .setOnAudioFocusChangeListener(listener, MAIN)
        .build();
    return manager.requestAudioFocus(focus);
  }

  public static int abandonFocus() {
    AudioManager manager = audioManager();
    if (manager == null || focus == null) {
      return 0;
    }
    int result = manager.abandonAudioFocusRequest(focus);
    focus = null;
    return result;
  }

  private static void focusChanged(int change) {
    if (change == AudioManager.AUDIOFOCUS_GAIN) {
      nativeEvent(DUCKED, 0);
      nativeEvent(INTERRUPTION_ENDED, 1);
    } else if (change == AudioManager.AUDIOFOCUS_LOSS) {
      nativeEvent(INTERRUPTION_BEGAN, 0);
    } else if (change == AudioManager.AUDIOFOCUS_LOSS_TRANSIENT) {
      nativeEvent(INTERRUPTION_BEGAN, 1);
    } else if (change == AudioManager.AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK) {
      nativeEvent(DUCKED, 1);
    }
  }

  // Headphones out, a Bluetooth device gone: "becoming noisy" is the lost route
  public static int observe() {
    Context context = context();
    if (context == null || observing) {
      return observing ? 1 : 0;
    }
    observing = true;
    BroadcastReceiver noisy = new BroadcastReceiver() {
      @Override
      public void onReceive(Context receiving, Intent intent) {
        if (AudioManager.ACTION_AUDIO_BECOMING_NOISY.equals(intent.getAction())) {
          nativeEvent(ROUTE_LOST, 0);
        }
      }
    };
    IntentFilter filter = new IntentFilter(AudioManager.ACTION_AUDIO_BECOMING_NOISY);
    if (Build.VERSION.SDK_INT >= 33) {
      context.registerReceiver(noisy, filter, Context.RECEIVER_EXPORTED);
    } else {
      context.registerReceiver(noisy, filter);
    }
    return 1;
  }

  // A MediaSession whose buttons become remote commands: `mask` as
  // rae_audio_remote_enable takes it (1 play, 2 pause, 4 toggle, 8 next,
  // 16 previous, 32 change position)
  public static int enable(int mask) {
    Context context = context();
    if (context == null) {
      return 0;
    }
    if (session != null) {
      return 1;
    }
    actions = 0;
    if ((mask & 1) != 0) actions |= PlaybackState.ACTION_PLAY;
    if ((mask & 2) != 0) actions |= PlaybackState.ACTION_PAUSE;
    if ((mask & 4) != 0) actions |= PlaybackState.ACTION_PLAY_PAUSE;
    if ((mask & 8) != 0) actions |= PlaybackState.ACTION_SKIP_TO_NEXT;
    if ((mask & 16) != 0) actions |= PlaybackState.ACTION_SKIP_TO_PREVIOUS;
    if ((mask & 32) != 0) actions |= PlaybackState.ACTION_SEEK_TO;
    session = new MediaSession(context, "rae");
    session.setCallback(new MediaSession.Callback() {
      @Override
      public void onPlay() {
        nativeRemote(PLAY, 0);
      }

      @Override
      public void onPause() {
        nativeRemote(PAUSE, 0);
      }

      @Override
      public void onSkipToNext() {
        nativeRemote(NEXT, 0);
      }

      @Override
      public void onSkipToPrevious() {
        nativeRemote(PREVIOUS, 0);
      }

      @Override
      public void onSeekTo(long position) {
        nativeRemote(CHANGE_POSITION, position);
      }
    }, MAIN);
    session.setPlaybackState(new PlaybackState.Builder().setActions(actions)
        .setState(PlaybackState.STATE_NONE, 0, 0f).build());
    session.setActive(true);
    return 1;
  }

  // What plays: the media notification's and the lock screen's text and
  // picture, and the state the buttons act on (rate 0 paused, 1 playing)
  public static int show(String title, String artist, String album, String artworkPath, long durationMs,
                         long positionMs, float rate) {
    if (session == null) {
      return 0;
    }
    MediaMetadata.Builder metadata = new MediaMetadata.Builder()
        .putString(MediaMetadata.METADATA_KEY_TITLE, title)
        .putString(MediaMetadata.METADATA_KEY_ARTIST, artist)
        .putString(MediaMetadata.METADATA_KEY_ALBUM, album);
    if (durationMs >= 0) {
      metadata.putLong(MediaMetadata.METADATA_KEY_DURATION, durationMs);
    }
    if (!artworkPath.isEmpty()) {
      Bitmap picture = BitmapFactory.decodeFile(artworkPath);
      if (picture != null) {
        metadata.putBitmap(MediaMetadata.METADATA_KEY_ART, picture);
      }
    }
    session.setMetadata(metadata.build());
    int state = rate > 0 ? PlaybackState.STATE_PLAYING : PlaybackState.STATE_PAUSED;
    session.setPlaybackState(new PlaybackState.Builder().setActions(actions)
        .setState(state, positionMs, rate).build());
    shown = String.format(java.util.Locale.ROOT, "%s|%s|%.2f|%.1f", title, artist, positionMs / 1000.0, rate);
    return 1;
  }

  public static void clear() {
    if (session != null) {
      session.setMetadata(null);
      session.setPlaybackState(new PlaybackState.Builder().setActions(actions)
          .setState(PlaybackState.STATE_STOPPED, 0, 0f).build());
    }
    shown = "||0.00|0.0";
  }

  public static String describe() {
    return shown;
  }
}
