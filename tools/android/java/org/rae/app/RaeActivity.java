// The app's activity: SDL's, plus the link that opened it (lib/AppLinks).
//
// SDLActivity passes on only a link's path, and at launch before the event
// queue exists, and it does nothing with a link that arrives while the app
// runs. So this class hands the whole URL to libmain.so, at both moments.
package org.rae.app;

import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;

import org.libsdl.app.SDLActivity;

public class RaeActivity extends SDLActivity {
  private static native void nativeOpenLink(String url);

  @Override
  protected void onCreate(Bundle savedInstanceState) {
    super.onCreate(savedInstanceState);
    // A restored activity still holds the link it was opened with: that one
    // was handed over before.
    if (savedInstanceState == null) {
      deliverLink(getIntent());
    }
  }

  @Override
  protected void onNewIntent(Intent intent) {
    super.onNewIntent(intent);
    setIntent(intent);
    deliverLink(intent);
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
