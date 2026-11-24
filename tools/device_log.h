/* The log file of an app on a phone (tools/device_log.c), for the launch shims
 * of tools/ios and tools/android. */
#ifndef RAE_DEVICE_LOG_H
#define RAE_DEVICE_LOG_H

/* Sends stdout and stderr to <folder>/rae.log, every line behind its local
 * time, and keeps the last runs' logs beside it. `line`, when given, gets each
 * whole line without the time (Android's logcat). 0 when the file could not be
 * opened; nothing is changed then. */
int device_log_start(const char* folder, void (*line)(const char* text));

/* What runs (the app, its build, the device), one or more whole lines: written
 * now, and again at the head of every further part of the log. */
void device_log_identity(const char* lines);

#endif
