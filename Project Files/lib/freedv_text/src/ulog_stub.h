/*  ulog_stub.h
 *
 *  Stand-in for FreeDV-GUI's "../util/logging/ulog.h" that rade_text.c
 *  uses for diagnostic prints.  Upstream's ulog is wxWidgets-coupled
 *  (calls into wxLog* from C++).  We don't need any of that here --
 *  the rade_text decoder already returns its result via the registered
 *  callback, so the log lines are pure dev diagnostics.
 *
 *  Compile-time: every macro expands to nothing, so the calls are
 *  zero-cost in the optimised build.  If you ever need to debug the
 *  decoder live, swap the body for printf or OutputDebugString.
 *
 *  Copyright (C) 2026  Christos Nikolaou (Thetis vendor)
 */

#ifndef _ulog_stub_h
#define _ulog_stub_h

#define log_debug(...) ((void)0)
#define log_info(...)  ((void)0)
#define log_warn(...)  ((void)0)
#define log_error(...) ((void)0)
#define log_fatal(...) ((void)0)
#define log_trace(...) ((void)0)

#endif /* _ulog_stub_h */
