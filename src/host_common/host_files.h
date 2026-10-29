// host_files.h — SD-card file explorer for the phone web app (both hosts).
//
// CMD "Z<seq>:<op>:<arg>" (phone.cpp queues it, loop() runs it here), every op answered on LOGDATA with
// one "FS=<n>:<seq>" header + <n> bytes ("OK\t…" / "DONE\t…" / "ERR\t<reason>"). <seq> (1-255) is the
// phone's request tag, echoed back so a reply that arrives after the phone gave up is dropped. Ops
// (shown without the tag):
//   Z:l:[<skip>:]<dir>   list a folder: "OK\t<dir>\n" then "<name>\t<size>\t<d|f>\t<mtime>\t<flag>\n"
//                        per entry (flag "live" = protected: live log / held / streaming); a full ~4 KB
//                        page ends "MORE\t<next skip>\n" and the phone asks again with that skip
//   Z:m:<dir>            make a folder
//   Z:r:<from>\t<to>     rename / move
//   Z:d:<path>           delete a file or an EMPTY folder
//   Z:u:<size>:<path>    begin an upload (written to <path>.upl, renamed over <path> when complete)
//   U<seq byte><bytes>   one upload chunk (tag = one raw byte) -> "OK\t<got>", the last one -> "DONE\t<size>"
//   Z:a:                 abort the upload
// Downloads reuse "F:<path>" (transferFile). Protected files (logPathProtected) and the "/" and
// "/logs" folders can't be deleted, renamed or overwritten.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace files {
constexpr size_t PATH_MAX_LEN = 96;    // longest accepted path + 1 (phone.cpp's F buffer is 100 B)
bool safePath(const char* p);          // absolute, < PATH_MAX_LEN, no ./../empty parts, no trailing '.'/' ',
                                       // control or FAT-illegal chars
bool isDir(const char* p);
bool uploadHolds(const char* p);       // p is the .upl temp file of the upload in progress
void service();                        // loop(): run a queued Z: op / upload chunk; times out a stalled upload
}
