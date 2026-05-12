/* Thetis-RADE stub for freedv-gui's util/sanitizers.h.
 *
 * The vendored WebRTC_AGC slice from freedv-gui's 3rdparty/ tree
 * resolves ../../util/sanitizers.h from agc.h.  Upstream that header
 * is a no-op on non-clang / non-rtsan builds; we recreate just the
 * empty-stub branch so agc.h parses cleanly under MSVC C and C++.
 */
#ifndef SANITIZERS_H
#define SANITIZERS_H

#define FREEDV_NONBLOCKING_EXCEPT
#ifdef __cplusplus
#define FREEDV_NONBLOCKING noexcept
#else
#define FREEDV_NONBLOCKING
#endif
#define FREEDV_BEGIN_VERIFIED_SAFE
#define FREEDV_END_VERIFIED_SAFE
#define FREEDV_BEGIN_REALTIME_UNSAFE {
#define FREEDV_END_REALTIME_UNSAFE }

#endif /* SANITIZERS_H */
