/* Real-mouse sweep for the windowed hover memory test (hover-memory-test-window.sh).
 *
 * Moves the REAL cursor down the centre column of the largest on-screen window
 * owned by <pid>, top to bottom in <sweepSeconds>, then jumps back to the top
 * and repeats for <seconds>. Finally it sends Cmd+Q to the process, so the app
 * quits through its normal path (SDL_EVENT_QUIT) and RAE_MEM_STATS prints.
 * The window bounds are re-read every 5 s, so moving the window is fine; moving
 * the mouse yourself during the run only disturbs the sweep for a moment.
 *
 *   mouse-sweep <pid> <seconds> <sweepSeconds>
 *
 * Posting mouse events needs Accessibility permission for the terminal that
 * runs it (System Settings > Privacy & Security > Accessibility). */
#include <ApplicationServices/ApplicationServices.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static int windowBounds(pid_t pid, CGRect* out) {
    CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID);
    int found = 0;
    double bestArea = 0;
    for (CFIndex i = 0; list && i < CFArrayGetCount(list); i++) {
        CFDictionaryRef window = CFArrayGetValueAtIndex(list, i);
        int owner = 0;
        CFNumberGetValue(CFDictionaryGetValue(window, kCGWindowOwnerPID), kCFNumberIntType, &owner);
        if (owner != pid) continue;
        CGRect bounds;
        CGRectMakeWithDictionaryRepresentation(CFDictionaryGetValue(window, kCGWindowBounds), &bounds);
        double area = bounds.size.width * bounds.size.height;
        if (area > bestArea) {
            bestArea = area;
            *out = bounds;
            found = 1;
        }
    }
    if (list) CFRelease(list);
    return found;
}

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: mouse-sweep <pid> <seconds> <sweepSeconds>\n");
        return 2;
    }
    pid_t pid = atoi(argv[1]);
    double seconds = atof(argv[2]), sweepSeconds = atof(argv[3]);
    CGRect bounds;
    int found = 0;
    for (int tries = 0; tries < 300 && !found; tries++) {
        found = windowBounds(pid, &bounds);
        if (!found) usleep(100000);
    }
    if (!found) {
        fprintf(stderr, "mouse-sweep: no window for pid %d\n", pid);
        return 1;
    }
    fprintf(stderr, "mouse-sweep: window %.0f,%.0f %.0fx%.0f\n", bounds.origin.x, bounds.origin.y,
            bounds.size.width, bounds.size.height);
    const int stepsPerSecond = 60;
    long sweepSteps = (long)(sweepSeconds * stepsPerSecond);
    long totalSteps = (long)(seconds * stepsPerSecond);
    if (sweepSteps < 2) sweepSteps = 2;
    for (long i = 0; i < totalSteps; i++) {
        if (i % (stepsPerSecond * 5) == 0) windowBounds(pid, &bounds);
        double x = bounds.origin.x + bounds.size.width / 2;
        double top = bounds.origin.y + 35;  /* below the title bar */
        double bottom = bounds.origin.y + bounds.size.height - 5;
        double y = top + (bottom - top) * (double)(i % sweepSteps) / (double)(sweepSteps - 1);
        CGEventRef move = CGEventCreateMouseEvent(NULL, kCGEventMouseMoved, CGPointMake(x, y),
                                                  kCGMouseButtonLeft);
        CGEventPost(kCGHIDEventTap, move);
        CFRelease(move);
        usleep(1000000 / stepsPerSecond);
    }
    CGEventRef keyDown = CGEventCreateKeyboardEvent(NULL, 12 /* q */, true);
    CGEventRef keyUp = CGEventCreateKeyboardEvent(NULL, 12, false);
    CGEventSetFlags(keyDown, kCGEventFlagMaskCommand);
    CGEventSetFlags(keyUp, kCGEventFlagMaskCommand);
    CGEventPostToPid(pid, keyDown);
    CGEventPostToPid(pid, keyUp);
    CFRelease(keyDown);
    CFRelease(keyUp);
    return 0;
}
