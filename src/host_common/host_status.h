// host_status.h — the CYD/XIAO -> phone streams: STATUS (key=val string) and DETS (live detection
// snapshot, v6). Carved from the CYD main.cpp (docs/mini-build.md §4.1).
#pragma once
void pushStatus();
void pushDetections();
