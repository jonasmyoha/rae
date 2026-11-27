// The app's activity: SDL's, plus the link that opened it (lib/AppLinks), the
// push notification whose tap opened it (lib/PushNotification), and the media
// keys handed to the system's media sessions.
//
// SDLActivity passes on only a link's path, and at launch before the event
// queue exists, and it does nothing with a link that arrives while the app
// runs. So this class hands the whole URL to libmain.so, at both moments.
package org.rae.app;

import android.content.Context;
import android.content.Intent;
import android.media.AudioManager;
import android.net.Uri;
import android.view.KeyEvent;
import android.os.Bundle;

import org.libsdl.app.SDLActivity;

public class RaeActivity extends SDLActivity {
  private static native void nativeOpenLink(String url);

  private static native void nativePushTapped(String payload);

  @Override
  protected void onCreate(Bundle savedInstanceState) {
    super.onCreate(savedInstanceState);
    // A restored activity still holds the link it was opened with: that one
    // was handed over before.
    if (savedInstanceState == null) {
      deliverLink(getIntent());
      deliverPushTap(getIntent());
    }
  }

  @Override
  protected void onNewIntent(Intent intent) {
    super.onNewIntent(intent);
    setIntent(intent);
    deliverLink(intent);
    deliverPushTap(intent);
  }

  // SDL turns every key into a key event of its own, the media keys too (a
  // headset's button, a keyboard's play key), so they never reached a
  // MediaSession while the app was in front (packages/audio's
  // RaeAudioSession). They go to the system's media key handling, as for an
  // app that does not take them.
  @Override
  public boolean dispatchKeyEvent(KeyEvent event) {
    if (isMediaKey(event.getKeyCode())) {
      AudioManager manager = (AudioManager) getSystemService(Context.AUDIO_SERVICE);
      if (manager != null) {
        manager.dispatchMediaKeyEvent(event);
        return true;
      }
    }
    return super.dispatchKeyEvent(event);
  }

  private static boolean isMediaKey(int code) {
    switch (code) {
      case KeyEvent.KEYCODE_MEDIA_PLAY:
      case KeyEvent.KEYCODE_MEDIA_PAUSE:
      case KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE:
      case KeyEvent.KEYCODE_HEADSETHOOK:
      case KeyEvent.KEYCODE_MEDIA_NEXT:
      case KeyEvent.KEYCODE_MEDIA_PREVIOUS:
      case KeyEvent.KEYCODE_MEDIA_STOP:
      case KeyEvent.KEYCODE_MEDIA_FAST_FORWARD:
      case KeyEvent.KEYCODE_MEDIA_REWIND:
        return true;
      default:
        return false;
    }
  }

  // A tap on a notification that Firebase showed (the app was in the background
  // or not running) starts this activity with the message's data keys as
  // extras, beside Firebase's own (google.*, gcm.*, from, collapse_key). The
  // data keys go on as a JSON object.
  private static void deliverPushTap(Intent intent) {
    Bundle extras = intent != null ? intent.getExtras() : null;
    if (extras == null || !extras.containsKey("google.message_id")) {
      return;
    }
    org.json.JSONObject payload = new org.json.JSONObject();
    for (String key : extras.keySet()) {
      if (key.startsWith("google.") || key.startsWith("gcm.") || key.equals("from") || key.equals("collapse_key")) {
        continue;
      }
      Object value = extras.get(key);
      try {
        payload.put(key, value != null ? value.toString() : org.json.JSONObject.NULL);
      } catch (org.json.JSONException error) {
        // a key the JSON cannot hold is left out
      }
    }
    try {
      nativePushTapped(payload.toString());
    } catch (UnsatisfiedLinkError error) {
      // the app does not use lib/PushNotification
    }
  }

  private static void deliverLink(Intent intent) {
    if (intent == null || !Intent.ACTION_VIEW.equals(intent.getAction())) {
      return;
    }
    Uri link = intent.getData();
    if (link == null) {
      return;
    }
    try {
      nativeOpenLink(link.toString());
    } catch (UnsatisfiedLinkError error) {
      // SDLActivity could not load libmain.so and shows its own error message
    }
  }
}
