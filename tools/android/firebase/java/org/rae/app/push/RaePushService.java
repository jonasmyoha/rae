// Firebase Cloud Messaging for lib/PushNotification (docs/push-notifications.md):
// the device token and the messages that arrive while the app runs, handed to
// libmain.so (runtime_push_notification.h). Compiled into an app only when its C
// includes that header and the build has the app's google-services.json
// (tools/android/gen-android.sh). The tap that opened the app is RaeActivity's.
package org.rae.app.push;

import com.google.firebase.messaging.FirebaseMessaging;
import com.google.firebase.messaging.FirebaseMessagingService;
import com.google.firebase.messaging.RemoteMessage;

import org.json.JSONObject;

import java.util.Map;

public class RaePushService extends FirebaseMessagingService {
  private static native void nativePushToken(String token);

  private static native void nativePushError(String error);

  private static native void nativePushArrived(String payload);

  // Asks FCM for this device's token; it comes to nativePushToken, or the reason
  // to nativePushError. Called from the Rae thread (rae_ext_PushNotification_register).
  public static boolean register() {
    try {
      FirebaseMessaging.getInstance().getToken().addOnCompleteListener(task -> {
        if (task.isSuccessful() && task.getResult() != null) {
          nativePushToken(task.getResult());
        } else {
          Exception error = task.getException();
          nativePushError(error != null ? error.toString() : "no token");
        }
      });
      return true;
    } catch (RuntimeException error) {
      // Firebase did not start (no google-services values for this package)
      nativePushError(error.toString());
      return false;
    }
  }

  @Override
  public void onNewToken(String token) {
    nativePushToken(token);
  }

  // A message while the app runs (the system shows nothing then): its data keys,
  // and its notification's title and body under "notification"
  @Override
  public void onMessageReceived(RemoteMessage message) {
    JSONObject payload = new JSONObject();
    try {
      for (Map.Entry<String, String> entry : message.getData().entrySet()) {
        payload.put(entry.getKey(), entry.getValue());
      }
      RemoteMessage.Notification notification = message.getNotification();
      if (notification != null) {
        JSONObject shown = new JSONObject();
        if (notification.getTitle() != null) shown.put("title", notification.getTitle());
        if (notification.getBody() != null) shown.put("body", notification.getBody());
        payload.put("notification", shown);
      }
    } catch (org.json.JSONException error) {
      // a key the JSON cannot hold is left out
    }
    nativePushArrived(payload.toString());
  }
}
