// Color emoji for lib/Gpu2dEmoji on Android (docs/text-emoji.md;
// compiler/runtime/runtime_emoji.h is the other half): one emoji cluster drawn by the
// system's emoji font into a square bitmap.
//
// Platform reason: the system's emoji font (Noto Color Emoji, or the maker's own) and its
// sequences, flags and skin tones are drawn by Android's text stack, which only Java reaches.
// The square's size, the cache and every fallback are Rae's.
package org.rae.app;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;

import java.nio.ByteBuffer;

public final class RaeEmoji {
    private RaeEmoji() {}

    // `text` centered in a `size` x `size` square: RGBA bytes, premultiplied alpha, rows from
    // the top; null when the system has no glyph for it
    public static byte[] draw(String text, int size) {
        if (text == null || text.isEmpty() || size <= 0) return null;
        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        // an emoji's box at a text size is about 1.17 em wide: this size fills the square
        paint.setTextSize(size / 1.17f);
        if (!paint.hasGlyph(text)) return null;
        Bitmap bitmap = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(bitmap);
        Paint.FontMetrics metrics = paint.getFontMetrics();
        float width = paint.measureText(text);
        float x = (size - width) * 0.5f;
        float y = (size - (metrics.descent - metrics.ascent)) * 0.5f - metrics.ascent;
        canvas.drawText(text, x, y, paint);
        ByteBuffer pixels = ByteBuffer.allocate(size * size * 4);
        bitmap.copyPixelsToBuffer(pixels);
        bitmap.recycle();
        return pixels.array();
    }
}
