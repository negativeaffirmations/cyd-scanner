# Handoff — 2026-10-02 (branch: phase5-dev)

## Done & pushed
Scan link-flicker RESOLVED. Commits on phase5-dev:
- `1ae02ea` scan-screen render split (updateScan partial repaint)
- `5de85d6` per-group srcMask (pause/filter no longer blanks list)
- `04875ca` requestScan kept atomic/synchronous
- `4d54393` **the fix**: tolerate C5 "busy misses"

Device runs `4d54393`. Tree clean, all diagnostics reverted.

## Root cause
C5 can't service the link during its Wi-Fi scan phase (brief busy window).
Render/timeout changes shifted CYD poll phase → every other poll phase-locked
into that window → got 0 bytes → CYD misread as link-down (red + blank + 5s stall).
Fix: requestScan() returns hit/miss, bails ~800ms on silence; on miss keep last
list, hold link green (sticky 8s), re-poll in ~400ms (breaks phase-lock).
Verified: C5 streams every 2s poll, no table=0.

## Open
- C5-side hardening (make scan not block link) NOT done — user said leave as-is.
- C5 scanner console = native USB 303A:1001 ("[C5] streamed N").

## Notes
- Flash: `PYTHONIOENCODING=utf-8 pio run -e cyd -t upload`. CYD=COM14 (CH340).
- Serial capture: use pyserial (pio monitor needs a TTY). dtr/rts=False to avoid
  reset; dtr=True to read C5 USB-CDC console. 15.4 test board (COM21) not a factor.
