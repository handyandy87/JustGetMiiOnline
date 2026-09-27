#pragma once

#include <cstdint>

// A record of what the app does to the filesystem, for working out where its shell keeps
// browser storage. Off unless the marker file is on the card.

// Looks for the marker and takes a buffer. Call once, from the application start hook.
void trace_start();

// Whether a buffer is open, which is the same question as whether tracing is on.
bool trace_on();

// True while the trace is writing itself out. A filesystem hook that logs has to skip whatever
// happens under this, or writing the file traces itself.
bool trace_busy();

// One line. Memory only, so a hook can call it without touching the filesystem.
void trace_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// One line of text the app produced, under a tag. Trailing newlines go, and the line stops at
// anything that reads like a credential.
void trace_text(const char *tag, const char *msg, uint32_t size);

// Writes what was collected to the card and lets the buffer go.
void trace_end();
