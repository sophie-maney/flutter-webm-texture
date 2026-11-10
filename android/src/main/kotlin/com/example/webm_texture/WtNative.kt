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

    /**
     * Creates the decoder for [bytes] or [path] at [width] x [height] (0 x 0
     * for the source size) and its render thread. Fills [out] (see `OUT_*`)
     * and returns the handle, or 0 on failure. [listener] receives
     * `onNativeError(code, message)` on the render thread.
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
        out: IntArray,
    ): Long

    /**
     * Draws into [surface] from now on and redraws the current frame; null
     * releases the current window before returning. Returns a `wt_status`.
     * Main thread.
     */
    @JvmStatic external fun nativeSetSurface(handle: Long, surface: Surface?): Int

    /** Main thread. */
    @JvmStatic external fun nativeSetActive(handle: Long, active: Boolean)

    /** Stops the render thread and frees everything. Main thread. */
    @JvmStatic external fun nativeDestroy(handle: Long)

    /** The core's name for a `wt_status`, such as `WT_ERR_LIMIT`. */
    @JvmStatic external fun nativeStatusName(status: Int): String
}

/** Receives failures from a native render thread. */
internal interface NativeErrorListener {
    /** Called on the render thread; must not block or call back into it. */
    fun onNativeError(code: String, message: String)
}
