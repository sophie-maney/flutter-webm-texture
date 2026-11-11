// The structs behind the opaque C handles of wt_api.h, for the files that
// implement the ABI (wt_api.cc, sched_api.cc). Nothing outside src/api
// includes this.
#ifndef WT_API_HANDLES_H_
#define WT_API_HANDLES_H_

#include <atomic>
#include <cstdint>
#include <memory>

#include "api/anim.h"
#include "api/wt_api.h"
#include "demux/source.h"

// The source owns its bytes and the limits every later probe/decode checks.
// It is shared by the caller's handle and every animation made from it, and
// freed when the last of them lets go.
struct wt_source {
  std::atomic<uint32_t> refs{1};
  std::unique_ptr<wt::ByteSource> bytes;
  wt_limits limits;
  wt_format format;  // Sniffed once at creation.
};

// An animation holds a reference to its source, whose bytes it reads.
struct wt_anim {
  wt_source* source = nullptr;
  std::unique_ptr<wt::Anim> anim;
};

#endif  // WT_API_HANDLES_H_
