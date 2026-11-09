package dev.webmtexture.spike_b

import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.os.Looper
import android.util.Log
import android.view.Surface
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.embedding.engine.renderer.FlutterRenderer
import io.flutter.plugin.common.MethodCall
import io.flutter.plugin.common.MethodChannel
import io.flutter.view.TextureRegistry
import java.util.concurrent.atomic.AtomicInteger

object SpikeNative {
    init {
        System.loadLibrary("spike_b")
    }

    @JvmStatic external fun attach(surface: Surface, w: Int, h: Int): Long

    @JvmStatic external fun release(handle: Long)

    @JvmStatic external fun drawRgba(handle: Long, rgba: ByteArray, w: Int, h: Int): Int

    @JvmStatic external fun drawAnim(handle: Long, w: Int, h: Int, frame: Int): Int
}

private const val TAG = "SpikeB"

/**
 * One SurfaceProducer plus a render thread. The Surface is fetched on the
 * platform thread (getSurface), then all ANativeWindow work happens on the
 * render thread.
 */
class SpikeTexture(
    private val producer: TextureRegistry.SurfaceProducer,
    val kind: String,
    private val w: Int,
    private val h: Int,
    private val rgba: ByteArray?,
) : TextureRegistry.SurfaceProducer.Callback {
    private val thread = HandlerThread("spike_b-$kind").apply { start() }
    private val render = Handler(thread.looper)
    private val main = Handler(Looper.getMainLooper())
    private var window = 0L // only touched on the render thread
    private var frame = 0
    private var animating = false

    val draws = AtomicInteger()
    val errors = AtomicInteger()
    val available = AtomicInteger()
    val cleanups = AtomicInteger()
    @Volatile var lastError = 0

    init {
        producer.setSize(w, h)
        producer.setCallback(this)
        attach(producer.surface)
    }

    val id: Long get() = producer.id()

    private fun attach(surface: Surface) {
        Log.i(TAG, "[$kind] attach surface valid=${surface.isValid}")
        render.post {
            if (window != 0L) SpikeNative.release(window)
            window = SpikeNative.attach(surface, w, h)
            if (kind == "static") {
                drawOnce()
            } else if (!animating) {
                animating = true
                render.post(tick)
            }
        }
    }

    private fun record(rc: Int) {
        if (rc == 0) draws.incrementAndGet() else {
            errors.incrementAndGet()
            lastError = rc
        }
    }

    private fun drawOnce() {
        if (window == 0L) return
        record(SpikeNative.drawRgba(window, rgba!!, w, h))
        // ImageReader-backed producers need a frame scheduled to pick up
        // the new image; harmless otherwise.
        main.post { producer.scheduleFrame() }
    }

    private val tick: Runnable = object : Runnable {
        override fun run() {
            if (!animating) return
            if (window != 0L) {
                record(SpikeNative.drawAnim(window, w, h, frame++))
                main.post { producer.scheduleFrame() }
            }
            render.postDelayed(this, 33)
        }
    }

    /** Redraws the static pattern into the current window (app resume). */
    fun redraw() {
        Log.i(TAG, "[$kind] redraw")
        if (kind == "static") render.post { drawOnce() }
    }

    override fun onSurfaceAvailable() {
        available.incrementAndGet()
        Log.i(TAG, "[$kind] onSurfaceAvailable")
        attach(producer.surface)
    }

    override fun onSurfaceCleanup() {
        cleanups.incrementAndGet()
        Log.i(TAG, "[$kind] onSurfaceCleanup")
        // The Surface is about to be destroyed: release the window
        // synchronously before returning, as the API requires.
        val done = java.util.concurrent.CountDownLatch(1)
        render.post {
            if (window != 0L) SpikeNative.release(window)
            window = 0L
            done.countDown()
        }
        done.await()
    }

    fun stats(): Map<String, Any> = mapOf(
        "draws" to draws.get(),
        "errors" to errors.get(),
        "lastError" to lastError,
        "available" to available.get(),
        "cleanups" to cleanups.get(),
    )
}

class MainActivity : FlutterActivity() {
    private val textures = mutableListOf<SpikeTexture>()

    override fun configureFlutterEngine(flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine)
        val registry = flutterEngine.renderer
        MethodChannel(flutterEngine.dartExecutor.binaryMessenger, "spike_b")
            .setMethodCallHandler { call, result -> handle(registry, call, result) }
    }

    private fun handle(registry: FlutterRenderer, call: MethodCall, result: MethodChannel.Result) {
        when (call.method) {
            "info" -> {
                val meta = packageManager
                    .getApplicationInfo(packageName, android.content.pm.PackageManager.GET_META_DATA)
                    .metaData
                result.success(
                    mapOf(
                        "platform" to "android",
                        "sdk" to Build.VERSION.SDK_INT,
                        "device" to "${Build.MANUFACTURER} ${Build.MODEL}",
                        "enableImpeller" to (meta?.get("io.flutter.embedding.android.EnableImpeller")?.toString() ?: "default"),
                        "impellerBackend" to (meta?.getString("io.flutter.embedding.android.ImpellerBackend") ?: "default"),
                    ),
                )
            }
            "create" -> {
                val kind = call.argument<String>("kind")!!
                val w = call.argument<Int>("width")!!
                val h = call.argument<Int>("height")!!
                val rgba = call.argument<ByteArray>("rgba")
                FlutterRenderer.debugForceSurfaceProducerGlTextures =
                    call.argument<Boolean>("forceGl") == true
                val lifecycle = if (call.argument<String>("lifecycle") == "reset") {
                    TextureRegistry.SurfaceLifecycle.resetInBackground
                } else {
                    TextureRegistry.SurfaceLifecycle.manual
                }
                val producer = registry.createSurfaceProducer(lifecycle)
                val t = SpikeTexture(producer, kind, w, h, rgba)
                textures.add(t)
                Log.i(TAG, "created $kind id=${t.id} producer=${producer.javaClass.simpleName}")
                result.success(
                    mapOf("textureId" to t.id, "path" to producer.javaClass.simpleName),
                )
            }
            "redraw" -> {
                textures.forEach { it.redraw() }
                result.success(null)
            }
            "stats" -> result.success(textures.associate { it.kind to it.stats() })
            else -> result.notImplemented()
        }
    }
}
