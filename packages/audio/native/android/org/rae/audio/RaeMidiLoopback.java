// A MIDI device whose input port loops back to its output port: what is sent
// to it comes back from it, through Android's MIDI service. For testing a
// program's MIDI (checks/audioMidi on Android, where a program has no virtual
// ports): the app declares it only when its build names the package part
// `midiLoopback` (RAE_ANDROID_PARTS, tools/android/gen-android.sh), so a
// normal app shows no extra MIDI device to other apps.
package org.rae.audio;

import android.media.midi.MidiDeviceService;
import android.media.midi.MidiReceiver;

import java.io.IOException;

public final class RaeMidiLoopback extends MidiDeviceService {
  private final MidiReceiver input = new MidiReceiver() {
    @Override
    public void onSend(byte[] message, int offset, int count, long timestamp) throws IOException {
      MidiReceiver[] outputs = getOutputPortReceivers();
      if (outputs != null && outputs.length > 0) {
        outputs[0].send(message, offset, count, timestamp);
      }
    }
  };

  @Override
  public MidiReceiver[] onGetInputPortReceivers() {
    return new MidiReceiver[] {input};
  }
}
