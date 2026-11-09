package dev.webmtexture.spike_c

import android.os.Build
import android.os.Debug
import android.os.Handler
import android.os.HandlerThread
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import android.view.Surface
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.embedding.engine.renderer.FlutterRenderer
import io.flutter.plugin.common.MethodCall
import io.flutter.plugin.common.MethodChannel
import io.flutter.view.TextureRegistry
import java.io.File
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executors

object SpikeNative {
    init {
        System.loadLibrary("spike_c")
    }

    @JvmStatic external fun setBank(bank: ByteArray, frames: Int, size: Int)

    @JvmStatic external fun attach(surface: Surface, w: Int, h: Int): Long

    @JvmStatic external fun release(handle: Long)

    @JvmStatic external fun tickTextures(windows: LongArray, tick: Int): Int

    @JvmStatic external fun tickAtlas(handle: Long, count: Int, tick: Int, gutter: Int): Int

    @JvmStatic external fun bufferStats(): IntArray

    @JvmStatic external fun phaseStats(reset: Boolean): LongArray

    @JvmStatic external fun progress(): Int
}

private const val TAG = "SpikeC"

/**
 * N stickers at [fps], drawn into either one SurfaceProducer per sticker
 * (`textures`) or one atlas SurfaceProducer (`atlas`). One render thread
 * does all ANativeWindow work, as the plugin's scheduler will. Producers use
 * `resetInBackground` (ADR 0005).
 */
class Grid(
    registry: FlutterRenderer,
    private val atlas: Boolean,
    private val count: Int,
    size: Int,
    atlasW: Int,
    atlasH: Int,
    fps: Int,
    private val gutter: Int,
) {
    private val thread = HandlerThread("spike_c-render").apply { start() }
    private val render = Handler(thread.looper)
    private val main = Handler(Looper.getMainLooper())
    private val periodNs = 1_000_000_000L / fps
    private val w = if (atlas) atlasW else size
    private val h = if (atlas) atlasH else size

    val producers: List<TextureRegistry.SurfaceProducer> =
        List(if (atlas) 1 else count) {
            registry.createSurfaceProducer(TextureRegistry.SurfaceLifecycle.resetInBackground)
        }

    // Render-thread state.
    private val windows = LongArray(producers.size)
    private var tick = 0
    private var nextNs = 0L
    private var running = false

    // Stats (render thread writes, read under lock).
    private val lock = Any()
    private var workMs = ArrayList<Double>(8192)
    private var lateMs = ArrayList<Double>(8192)
    private var errors = 0
    private var notifies = 0
    private var renderCpuNs = 0L

    init {
        producers.forEachIndexed { i, p ->
            p.setSize(w, h)
            p.setCallback(object : TextureRegistry.SurfaceProducer.Callback {
                override fun onSurfaceAvailable() {
                    Log.i(TAG, "[$i] onSurfaceAvailable")
                    attach(i, p.surface)
                }

                override fun onSurfaceCleanup() {
                    Log.i(TAG, "[$i] onSurfaceCleanup")
                    val done = CountDownLatch(1)
                    render.post {
                        if (windows[i] != 0L) SpikeNative.release(windows[i])
                        windows[i] = 0L
                        done.countDown()
                    }
                    done.await()
                }
            })
            attach(i, p.surface)
        }
    }

    /**
     * Starts the tick loop. Not done in `init`: the render thread could run
     * before the `loop`/`watchdog` properties below are initialised and post
     * a null Runnable, which silently never runs.
     */
    fun start() {
        render.post {
            running = true
            nextNs = System.nanoTime()
            render.post(loop)
        }
        main.postDelayed(watchdog, 5000)
    }

    val ids: List<Long> get() = producers.map { it.id() }

    // Watchdog: if the render thread stops completing ticks, log where it is.
    @Volatile private var lastTickNs = System.nanoTime()
    private var stalls = 0
    private val watchdog = object : Runnable {
        override fun run() {
            val idle = (System.nanoTime() - lastTickNs) / 1_000_000
            if (idle > 3000) {
                stalls++
                val p = SpikeNative.progress()
                Log.w(TAG, "STALL ${idle}ms tick=$tick phase=${p / 1000} window=${p % 1000} " +
                    "stack=${thread.stackTrace.take(6).joinToString(" < ")}")
            }
            main.postDelayed(this, 2000)
        }
    }

    private fun attach(i: Int, surface: Surface) {
        render.post {
            if (windows[i] != 0L) SpikeNative.release(windows[i])
            windows[i] = SpikeNative.attach(surface, w, h)
        }
    }

    private val scheduleAll = Runnable { producers.forEach { it.scheduleFrame() } }

    private val loop: Runnable = object : Runnable {
        override fun run() {
            if (!running) return
            val start = System.nanoTime()
            val cpu0 = Debug.threadCpuTimeNanos()
            val failed = if (producers.isEmpty()) {
                0
            } else if (atlas) {
                SpikeNative.tickAtlas(windows[0], count, tick, gutter)
            } else {
                SpikeNative.tickTextures(windows, tick)
            }
            // One main-thread hop per tick, scheduling every producer.
            if (producers.isNotEmpty()) main.post(scheduleAll)
            val end = System.nanoTime()
            val cpu1 = Debug.threadCpuTimeNanos()
            synchronized(lock) {
                workMs.add((end - start) / 1e6)
                lateMs.add((start - nextNs) / 1e6)
                if (failed != 0) errors++
                notifies += producers.size
                renderCpuNs += cpu1 - cpu0
            }
            tick++
            lastTickNs = end
            // Fixed-rate schedule; skip ticks rather than bunch them up.
            nextNs += periodNs
            while (nextNs < end) nextNs += periodNs
            render.postAtTime(this, (nextNs - System.nanoTime()) / 1_000_000 + SystemClock.uptimeMillis())
        }
    }

    fun resetStats() = synchronized(lock) {
        workMs = ArrayList(8192)
        lateMs = ArrayList(8192)
        errors = 0
        notifies = 0
        renderCpuNs = 0
        SpikeNative.phaseStats(true)
    }

    fun stats(): Map<String, Any> = synchronized(lock) {
        mapOf(
            "tickWorkMs" to workMs.toDoubleArray(),
            "tickLateMs" to lateMs.toDoubleArray(),
            "errors" to errors,
            "notifies" to notifies,
            "extra" to SpikeNative.bufferStats().let { b ->
              mapOf(
                "renderThreadCpuMs" to renderCpuNs / 1e6,
                "stalls" to stalls,
                "distinctBuffers" to b[0],
                "maxBuffersPerWindow" to b[1],
                "windowsSeen" to b[2],
                "bufferBytes" to w.toLong() * h * 4,
                "phaseMsPerCall" to SpikeNative.phaseStats(false).let { p ->
                    val n = maxOf(p[3], 1L).toDouble()
                    mapOf("lock" to p[0] / 1e6 / n, "copy" to p[1] / 1e6 / n, "post" to p[2] / 1e6 / n, "calls" to p[3])
                },
                "producer" to (producers.firstOrNull()?.javaClass?.simpleName ?: "none"),
              )
            },
        )
    }
}

class MainActivity : FlutterActivity() {
    private var grid: Grid? = null
    private val sampler = Executors.newSingleThreadExecutor()
    private val main = Handler(Looper.getMainLooper())

    override fun configureFlutterEngine(flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine)
        val registry = flutterEngine.renderer
        MethodChannel(flutterEngine.dartExecutor.binaryMessenger, "spike_c")
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
                        "cores" to Runtime.getRuntime().availableProcessors(),
                        "enableImpeller" to (meta?.get("io.flutter.embedding.android.EnableImpeller")?.toString() ?: "default"),
                        "impellerBackend" to (meta?.getString("io.flutter.embedding.android.ImpellerBackend") ?: "default"),
                    ),
                )
            }
            "start" -> {
                val bank = call.argument<ByteArray>("bank")!!
                SpikeNative.setBank(bank, call.argument<Int>("frames")!!, call.argument<Int>("size")!!)
                val g = Grid(
                    registry,
                    atlas = call.argument<String>("mode") == "atlas",
                    count = call.argument<Int>("count")!!,
                    size = call.argument<Int>("size")!!,
                    atlasW = call.argument<Int>("atlasW")!!,
                    atlasH = call.argument<Int>("atlasH")!!,
                    fps = call.argument<Int>("fps")!!,
                    gutter = call.argument<Int>("gutter")!!,
                )
                grid = g
                g.start()
                val producer = g.producers.firstOrNull()?.javaClass?.simpleName ?: "none"
                Log.i(TAG, "started ${g.producers.size} producers ($producer)")
                result.success(
                    mapOf(
                        "textureIds" to g.ids,
                        "producer" to producer,
                    ),
                )
            }
            "resetStats" -> {
                grid?.resetStats()
                result.success(null)
            }
            "stats" -> result.success(grid?.stats() ?: emptyMap<String, Any>())
            // Memory and CPU, sampled off the main thread: Debug.getMemoryInfo
            // reads smaps and would stall the (merged) platform/UI thread.
            "sample" -> sampler.execute {
                val s = sample()
                main.post { result.success(s) }
            }
            else -> result.notImplemented()
        }
    }

    private fun sample(): Map<String, Any> {
        val mi = Debug.MemoryInfo()
        Debug.getMemoryInfo(mi)
        fun stat(k: String) = mi.getMemoryStat(k)?.toLongOrNull() ?: -1L
        val status = File("/proc/self/status").readLines()
        fun kb(k: String) = status.firstOrNull { it.startsWith("$k:") }
            ?.split(Regex("\\s+"))?.getOrNull(1)?.toLongOrNull() ?: -1L
        // utime + stime, in clock ticks (100 Hz on Android).
        val f = File("/proc/self/stat").readText().substringAfterLast(')').trim().split(" ")
        val cpuMs = (f[11].toLong() + f[12].toLong()) * 10
        return mapOf(
            "cpuMs" to cpuMs,
            "rssKb" to kb("VmRSS"),
            "pssKb" to stat("summary.total-pss"),
            "graphicsKb" to stat("summary.graphics"),
            "nativeHeapKb" to stat("summary.native-heap"),
        )
    }
}
