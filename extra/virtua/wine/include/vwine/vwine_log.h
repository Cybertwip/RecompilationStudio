// vwine_log.h — the guest layer's report channel.
//
// This is stderr, which MVII copies into the app log and out the serial
// console. It is the same channel gba-to-mvii's runtime uses (gbamvii::logf)
// and it is used the same way: startup, load decisions, and failures. Never per
// frame and never per guest call -- the serial path is a byte at a time, and a
// trace on a hot path costs more than the work it describes.
//
// A loader that fails silently is indistinguishable from a process that
// vanished, which is precisely the failure mode the /dev/native0 mock had. So
// every refusal in this layer says which file, which offset, and which field.

#ifndef VWINE_LOG_H
#define VWINE_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

void vwine_logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

#ifdef __cplusplus
}
#endif

#endif  // VWINE_LOG_H
