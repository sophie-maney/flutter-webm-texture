package com.example.webm_texture

import android.util.Log
import io.flutter.view.TextureRegistry

private const val TAG = "WebmTexture"

/**
 * One animation shown through one `SurfaceProducer` (T2.2).
 *
 * The native player (decoder plus render thread) is created first, off the
 * main thread; [attach] then gives it the producer's surface. Every method
 * here runs on the platform main thread.
 *
 * Lifecycle rules from spike B ([ADR 0005](docs/decisions/0005-no-ui-image-fallback.md)):
 * the producer uses `SurfaceLifecycle.resetInBackground`; [onSurfaceCleanup]
 * releases the native window before returning; [onSurfaceAvailable] fetches
 * the new surface and the native side redraws the current frame into it.
 */
internal class StickerTexture(
    val animId: Long,
    private val onError: (animId: Long, code: String, message: String) -> Unit,
) : TextureRegistry.SurfaceProducer.Callback, NativeErrorListener {
    private var handle = 0L
    private var producer: TextureRegistry.SurfaceProducer? = null

    val textureId: Long get() = producer?.id() ?: -1

    /**
     * Creates the native player. Returns `{status, width, height, frames}`
     * (see `WtNative.OUT_*`). Background thread; call once, before [attach].
     */
    fun createNative(source: AnimationSourceMessage, width: Int, height: Int, loop: Boolean): IntArray {
        val out = IntArray(WtNative.OUT_LENGTH)
        handle = WtNative.nativeCreate(this, source.bytes, source.path, width, height, loop, out)
        return out
    }

    /** Hands the producer's surface to the native player. */
    fun attach(producer: TextureRegistry.SurfaceProducer, width: Int, height: Int) {
        this.producer = producer
        producer.setSize(width, height)
        producer.setCallback(this)
        // SurfaceTexture-backed producers render nothing under Impeller
        // Vulkan (spike B finding 4); the class name makes that diagnosable.
        Log.i(
            TAG,
            "anim $animId: ${producer.javaClass.simpleName} id=${producer.id()} ${width}x$height",
        )
        setSurface()
    }

    fun setActive(active: Boolean) {
        if (handle != 0L) WtNative.nativeSetActive(handle, active)
    }

    /** Shows [frame] next; returns a `wt_status`. */
    fun seekToFrame(frame: Int): Int =
        if (handle != 0L) WtNative.nativeSeek(handle, frame) else WtNative.WT_OK

    /** Stops drawing, then releases the producer. Safe to call twice. */
    fun dispose() {
        producer?.setCallback(null)
        if (handle != 0L) {
            // Joins the render thread and releases the window, so nothing
            // writes to the surface once the producer is released below.
            WtNative.nativeDestroy(handle)
            handle = 0L
        }
        producer?.release()
        producer = null
    }

    override fun onSurfaceAvailable() {
        Log.i(TAG, "anim $animId: onSurfaceAvailable")
        setSurface()
    }

    override fun onSurfaceCleanup() {
        Log.i(TAG, "anim $animId: onSurfaceCleanup")
        // The surface is destroyed after this returns, so the native window
        // must be released synchronously (waits for at most one frame write).
        if (handle != 0L) WtNative.nativeSetSurface(handle, null)
    }

    override fun onNativeError(code: String, message: String) = onError(animId, code, message)

    private fun setSurface() {
        val p = producer ?: return
        if (handle == 0L) return
        // Re-fetch every time: after a cleanup the producer has a new surface.
        val status = WtNative.nativeSetSurface(handle, p.surface)
        if (status != WtNative.WT_OK) {
            onError(animId, "SURFACE_LOST", "attaching the surface failed: ${WtNative.nativeStatusName(status)}")
        }
    }
}
