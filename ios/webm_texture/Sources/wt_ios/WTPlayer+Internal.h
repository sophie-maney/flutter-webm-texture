// What WTAtlas needs from WTPlayer and the public header doesn't show: the
// scheduler handle, which wt_atlas_attach and wt_atlas_detach take.
#import "WTPlayer.h"

#include "wt_api.h"

NS_ASSUME_NONNULL_BEGIN

/// The process-wide scheduler every player and atlas runs on, created on
/// first use; NULL if it couldn't start. Any thread.
wt_sched *_Nullable WTSharedScheduler(void);

@interface WTPlayer (Internal)

/// Runs `block` with the player's handle (NULL after -close) under the
/// player's lock, so -close can't overlap it, and returns what it returns.
- (int32_t)withPlayerHandle:(int32_t (^NS_NOESCAPE)(wt_player *_Nullable player))block;

@end

NS_ASSUME_NONNULL_END
