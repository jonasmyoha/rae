// packages/audio's Android MIDI (docs/audio-design.md §8.2): the devices
// MidiManager knows (USB, Bluetooth, other apps' virtual devices), their
// names and ports, and opening one. native/rae_audio_glue.c reads and writes
// an opened device's ports with the NDK's AMidi, into the queue
// audio/MidiPorts polls. Opening is asynchronous: an opened device is kept
// here by its id until C takes it.
//
// Platform reason: listing and opening MIDI devices is Java-only
// (android.media.midi); AMidi starts from a Java MidiDevice.
package org.rae.audio;

import android.content.Context;
import android.content.pm.PackageManager;
import android.media.midi.MidiDevice;
import android.media.midi.MidiDeviceInfo;
import android.media.midi.MidiManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;

import java.util.HashMap;
import java.util.Map;

import org.libsdl.app.SDLActivity;

public final class RaeAudioMidi {
  private static final Handler MAIN = new Handler(Looper.getMainLooper());
  private static MidiManager manager;
  private static MidiDeviceInfo[] devices = new MidiDeviceInfo[0];
  private static final Map<Integer, MidiDevice> opened = new HashMap<>();

  // 1 when the system has MIDI
  public static int available() {
    Context context = SDLActivity.getContext();
    if (context == null || !context.getPackageManager().hasSystemFeature(PackageManager.FEATURE_MIDI)) {
      return 0;
    }
    manager = (MidiManager) context.getSystemService(Context.MIDI_SERVICE);
    return manager == null ? 0 : 1;
  }

  // Look again; how many devices there are now
  public static int refresh() {
    devices = manager == null ? new MidiDeviceInfo[0] : manager.getDevices();
    return devices.length;
  }

  private static MidiDeviceInfo info(int index) {
    return index >= 0 && index < devices.length ? devices[index] : null;
  }

  public static int deviceId(int index) {
    MidiDeviceInfo info = info(index);
    return info == null ? -1 : info.getId();
  }

  public static String deviceName(int index) {
    MidiDeviceInfo info = info(index);
    if (info == null) {
      return "";
    }
    Bundle properties = info.getProperties();
    String name = properties.getString(MidiDeviceInfo.PROPERTY_NAME);
    if (name == null) {
      name = properties.getString(MidiDeviceInfo.PROPERTY_PRODUCT);
    }
    return name == null ? "MIDI device " + info.getId() : name;
  }

  // Ports that send to us (a keyboard's) and that we send to (a synth's)
  public static int outputPorts(int index) {
    MidiDeviceInfo info = info(index);
    return info == null ? 0 : info.getOutputPortCount();
  }

  public static int inputPorts(int index) {
    MidiDeviceInfo info = info(index);
    return info == null ? 0 : info.getInputPortCount();
  }

  // Open the device with this id; it is kept once the system has opened it
  public static int open(int id) {
    if (manager == null) {
      return 0;
    }
    for (MidiDeviceInfo info : devices) {
      if (info.getId() == id) {
        synchronized (opened) {
          if (opened.containsKey(id)) {
            return 1;
          }
        }
        manager.openDevice(info, device -> {
          if (device != null) {
            synchronized (opened) {
              opened.put(id, device);
            }
          }
        }, MAIN);
        return 1;
      }
    }
    return 0;
  }

  // The opened device with this id, or null while it is still opening
  public static Object device(int id) {
    synchronized (opened) {
      return opened.get(id);
    }
  }
}
