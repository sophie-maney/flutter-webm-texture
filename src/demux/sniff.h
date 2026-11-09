// Format detection from magic bytes.
//
// Sniffing only picks a demuxer; it validates nothing beyond the signature.
// The demuxer decides whether the rest of the file is playable (a Matroska
// file that isn't WebM, a still WebP, or a truncated GIF all sniff as their
// container and are rejected later).
#ifndef WT_DEMUX_SNIFF_H_
#define WT_DEMUX_SNIFF_H_

#include "api/wt_api.h"
#include "demux/source.h"

namespace wt {

// Returns WT_FORMAT_WEBM (EBML header 1A 45 DF A3), WT_FORMAT_WEBP ("RIFF",
// four size bytes, "WEBP"), WT_FORMAT_GIF ("GIF87a" or "GIF89a"), or
// WT_FORMAT_UNKNOWN for anything else, including empty or too-short input.
// Reads at most the first 12 bytes. Thread: any thread.
wt_format Sniff(ByteView bytes);

}  // namespace wt

#endif  // WT_DEMUX_SNIFF_H_
