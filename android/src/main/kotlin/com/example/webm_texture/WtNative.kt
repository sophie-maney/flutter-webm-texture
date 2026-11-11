package com.example.webm_texture

import android.view.Surface

/**
 * JNI bindings to the texture glue in `src/main/cpp/jni_bridge.cc`, which
 * lives in `libwebm_texture.so` next to the core (the same library Dart opens
 * over FFI).
 */
internal object WtNative {
    init {
        System.loadLibrary("webm_texture")
    }

    // Indices into nativeCreate's `out` array.
    const val OUT_STATUS = 0
    const val OUT_WIDTH = 1
    const val OUT_HEIGHT = 2
    const val OUT_FRAMES = 3
    const val OUT_LENGTH = 4

    const val WT_OK = 0
    const val WT_ERR_INVALID_ARGUMENT = -1

    /**
     * Creates the decoder for [bytes] or [path] at [width] x [height] (0 x 0
     * for the source size) and puts it on the process-wide scheduler, which
     * draws its first frame once it has a surface. With [loop] false,
     * playback stops on the last frame. With a [cacheDir] and a [sourceKey]
     * (and a non-zero size) the frames are cached in a file named after the
     * key and size there; the directory must exist. Fills [out] (see `OUT_*`)
     * and returns the handle, or 0 on failure. [listener] receives
     * `onNativeError(code, message)` on a scheduler worker thread.
     *
     * Background thread only: it reads and probes the source.
     */
    @JvmStatic
    external fun nativeCreate(
        listener: NativeErrorListener,
        bytes: ByteArray?,
        path: String?,
        width: Int,
        height: Int,
        loop: Boolean,
        cacheDir: String?,
        sourceKey: String?,
        out: IntArray,
    ): Long

    /**
     * Deletes least recently used cache files in [cacheDir] until it fits the
     * disk budget, and stray temporaries. Returns a `wt_status`. Background
     * thread only: it reads the directory.
     */
    @JvmStatic external fun nativeEvictCache(cacheDir: String): Int

    /**
     * Draws into [surface] from now on and redraws the current frame; null
     * releases the current window before returning. Returns a `wt_status`.
     * Main thread.
     */
    @JvmStatic external fun nativeSetSurface(handle: Long, surface: Surface?): Int

    /** Main thread. */
    @JvmStatic external fun nativeSetActive(handle: Long, active: Boolean)

    /**
     * The on-screen area, in physical pixels, of the widgets playing the
     * animation: its rank under the scheduler's `maxConcurrent` limit. Main
     * thread.
     */
    @JvmStatic external fun nativeSetPriority(handle: Long, priority: Long)

    /**
     * Shows [frame] next, playing or paused. Returns a `wt_status`
     * (`WT_ERR_INVALID_ARGUMENT` outside the animation's frames). Main thread.
     */
    @JvmStatic external fun nativeSeek(handle: Long, frame: Int): Int

    /**
     * Takes the animation off the scheduler and frees everything; waits for a
     * frame in flight. Main thread.
     */
    @JvmStatic external fun nativeDestroy(handle: Long)

    /**
     * Applies the device tier's tunables to the process-wide scheduler:
     * 1..8 workers, 1..240 fps, and a concurrency limit (0 for none).
     * Returns a `wt_status`. Main thread.
     */
    @JvmStatic external fun nativeSetSchedulerOptions(workerCount: Int, maxFps: Int, maxConcurrent: Int): Int

    /** The core's name for a `wt_status`, such as `WT_ERR_LIMIT`. */
    @JvmStatic external fun nativeStatusName(status: Int): String
}

/** Receives failures from the native scheduler's worker threads. */
internal interface NativeErrorListener {
    /** Called on a worker thread; must not block or call back into the player. */
    fun onNativeError(code: String, message: String)
}
