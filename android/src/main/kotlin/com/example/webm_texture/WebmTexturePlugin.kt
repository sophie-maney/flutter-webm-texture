package com.example.webm_texture

import android.os.Handler
import android.os.Looper
import io.flutter.embedding.engine.plugins.FlutterPlugin
import io.flutter.plugin.common.MethodCall
import io.flutter.plugin.common.MethodChannel
import io.flutter.plugin.common.MethodChannel.MethodCallHandler
import io.flutter.plugin.common.MethodChannel.Result
import io.flutter.view.TextureRegistry
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File

private const val CACHE_DIR_NAME = "webm_texture"

// Retired textures kept at most; older ones are released even if Dart never
// asked.
private const val MAX_RETIRED = 64

/**
 * The Android side of the Pigeon API ([ADR 0010](docs/decisions/0010-pigeon-api-shape.md)).
 * Each animation is a [StickerTexture]: a `SurfaceProducer` filled by the
 * native scheduler's workers. Host API calls arrive on the platform main
 * thread.
 */
class WebmTexturePlugin :
    FlutterPlugin,
    MethodCallHandler,
    WebmTextureHostApi {
    private lateinit var channel: MethodChannel
    private var errorChannel: StreamErrorsEventChannel? = null
    private var signalsChannel: StreamDeviceSignalsEventChannel? = null
    private var deviceSignals: DeviceSignals? = null
    private var textures: TextureRegistry? = null
    private val main = Handler(Looper.getMainLooper())

    // Main thread only.
    private val animations = HashMap<Long, StickerTexture>()
    private var nextAnimId = 1L
    private var errorSink: PigeonEventSink<AnimationErrorMessage>? = null
    private val atlases = HashMap<Long, StickerAtlas>()
    private var nextAtlasId = 1L

    // Textures Dart may still draw for a frame or two after they were
    // replaced (an atlas page's old size, an animation's own texture once it
    // entered an atlas), oldest first, until releaseTextures. Main thread.
    private val retired = LinkedHashMap<Long, TextureRegistry.SurfaceProducer>()

    // The frame cache directory, set on attach and by configureCache; read
    // from IO threads, so volatile. Created on first use (createAnimation).
    @Volatile private var cacheDir: File? = null
    private val background = CoroutineScope(SupervisorJob() + Dispatchers.IO)

    override fun onAttachedToEngine(flutterPluginBinding: FlutterPlugin.FlutterPluginBinding) {
        textures = flutterPluginBinding.textureRegistry
        val signals = DeviceSignals(flutterPluginBinding.applicationContext)
        deviceSignals = signals
        val dir = File(flutterPluginBinding.applicationContext.cacheDir, CACHE_DIR_NAME)
        cacheDir = dir
        // Keep the cache within its budget from the start; frame writes
        // keep it there afterwards.
        background.launch { WtNative.nativeEvictCache(dir.path) }
        channel = MethodChannel(flutterPluginBinding.binaryMessenger, "webm_texture")
        channel.setMethodCallHandler(this)
        WebmTextureHostApi.setUp(flutterPluginBinding.binaryMessenger, this)
        signalsChannel = StreamDeviceSignalsStreamHandler.register(
            flutterPluginBinding.binaryMessenger,
            object : StreamDeviceSignalsStreamHandler() {
                override fun onListen(p0: Any?, sink: PigeonEventSink<DeviceSignalsMessage>) {
                    signals.listen { sink.success(signals.read()) }
                }

                override fun onCancel(p0: Any?) {
                    signals.stop()
                }
            },
        )
        errorChannel = StreamErrorsStreamHandler.register(
            flutterPluginBinding.binaryMessenger,
            object : StreamErrorsStreamHandler() {
                override fun onListen(p0: Any?, sink: PigeonEventSink<AnimationErrorMessage>) {
                    errorSink = sink
                }

                override fun onCancel(p0: Any?) {
                    errorSink = null
                }
            },
        )
    }

    override suspend fun createAnimation(
        sourceKey: String,
        source: AnimationSourceMessage,
        bucketW: Long,
        bucketH: Long,
        loop: Boolean
    ): AnimationHandleMessage {
        if ((source.bytes == null) == (source.path == null)) {
            throw FlutterError("WT_ERR_INVALID_ARGUMENT", "set exactly one of bytes and path", null)
        }
        if (bucketW !in 0..Int.MAX_VALUE || bucketH !in 0..Int.MAX_VALUE) {
            throw FlutterError("WT_ERR_INVALID_ARGUMENT", "bucket ${bucketW}x$bucketH out of range", null)
        }
        val dir = cacheDir
        if (dir == null) {
            throw FlutterError("ENGINE_DETACHED", "the Flutter engine was detached", null)
        }
        val animId = nextAnimId++
        val texture = StickerTexture(animId, ::postError)

        // Probing and decoder setup read the file, so keep them off main.
        val out = withContext(Dispatchers.IO) {
            // The core writes cache files here but never creates the directory.
            dir.mkdirs()
            texture.createNative(source, bucketW.toInt(), bucketH.toInt(), loop, dir.path, sourceKey)
        }
        val status = out[WtNative.OUT_STATUS]
        if (status != WtNative.WT_OK) {
            throw FlutterError(WtNative.nativeStatusName(status), "can't play $sourceKey", null)
        }

        val registry = textures
        if (registry == null) {
            texture.dispose()
            throw FlutterError("ENGINE_DETACHED", "the Flutter engine was detached", null)
        }
        val producer = try {
            registry.createSurfaceProducer(TextureRegistry.SurfaceLifecycle.resetInBackground)
        } catch (e: RuntimeException) {
            // "Cannot execute operation because FlutterJNI is not attached to
            // native" when the engine is detaching (spike B).
            texture.dispose()
            throw FlutterError("ENGINE_DETACHED", e.message, null)
        }
        texture.attach(producer, out[WtNative.OUT_WIDTH], out[WtNative.OUT_HEIGHT])
        animations[animId] = texture
        return AnimationHandleMessage(animId, texture.textureId)
    }

    override fun setActive(animId: Long, active: Boolean) {
        animations[animId]?.setActive(active)
    }

    override fun setPriority(animId: Long, priority: Long) {
        animations[animId]?.setPriority(priority)
    }

    override fun seekToFrame(animId: Long, frame: Long) {
        val texture = animations[animId] ?: return
        val status =
            if (frame !in 0..Int.MAX_VALUE) WtNative.WT_ERR_INVALID_ARGUMENT else texture.seekToFrame(frame.toInt())
        if (status != WtNative.WT_OK) {
            throw FlutterError(WtNative.nativeStatusName(status), "can't seek to frame $frame", null)
        }
    }

    override fun dispose(animId: Long) {
        val anim = animations.remove(animId) ?: return
        // Destroying the player frees its atlas slot (wt_player_destroy).
        anim.atlas?.members?.remove(animId)
        anim.atlas = null
        anim.dispose()
    }

    override fun createAtlas(options: AtlasOptionsMessage): Long {
        if (options.width !in 16..2048 || options.maxHeight !in 16..2048 || options.maxPages !in 1..16) {
            throw FlutterError("WT_ERR_INVALID_ARGUMENT", "atlas options out of range: $options", null)
        }
        val out = IntArray(1)
        val handle = WtNative.nativeAtlasCreate(
            options.width.toInt(), options.maxHeight.toInt(), options.maxPages.toInt(), out,
        )
        if (handle == 0L) {
            throw FlutterError(WtNative.nativeStatusName(out[0]), "can't create the atlas", null)
        }
        val atlasId = nextAtlasId++
        atlases[atlasId] = StickerAtlas(atlasId, handle, ::retire)
        return atlasId
    }

    override fun attachToAtlas(atlasId: Long, animIds: List<Long>): AtlasLayoutMessage {
        val atlas = atlases[atlasId]
            ?: throw FlutterError("WT_ERR_INVALID_ARGUMENT", "unknown atlas $atlasId", null)
        val registry = textures
            ?: throw FlutterError("ENGINE_DETACHED", "the Flutter engine was detached", null)
        val retiredIds = ArrayList<Long>()
        val out = IntArray(WtNative.SLOT_LENGTH)
        val slots = animIds.map { animId ->
            val anim = animations[animId]
            val status = when {
                anim == null || anim.atlas != null -> WtNative.WT_ERR_INVALID_ARGUMENT
                else -> atlas.attach(anim, out)
            }
            if (anim == null || status != WtNative.WT_OK) {
                AtlasSlotMessage(animId, -1, 0, 0, 0, 0, WtNative.nativeStatusName(status))
            } else {
                anim.atlas = atlas
                atlas.members += animId
                // Its frames go to the slot now; its own texture keeps the
                // last one until Dart has switched.
                anim.retireProducer()?.let {
                    retiredIds += it.id()
                    retire(it)
                }
                AtlasSlotMessage(
                    animId,
                    out[WtNative.SLOT_PAGE].toLong(),
                    out[WtNative.SLOT_X].toLong(),
                    out[WtNative.SLOT_Y].toLong(),
                    out[WtNative.SLOT_WIDTH].toLong(),
                    out[WtNative.SLOT_HEIGHT].toLong(),
                )
            }
        }
        retiredIds += atlas.sync(registry)
        return AtlasLayoutMessage(slots, emptyList(), atlas.pageMessages(), retiredIds)
    }

    override fun detachFromAtlas(animIds: List<Long>): AtlasLayoutMessage {
        val registry = textures
            ?: throw FlutterError("ENGINE_DETACHED", "the Flutter engine was detached", null)
        val touched = LinkedHashSet<StickerAtlas>()
        val handles = animIds.map { animId ->
            val anim = animations[animId] ?: return@map AnimationHandleMessage(animId, -1)
            val atlas = anim.atlas
            if (atlas != null && atlas.detach(anim) == WtNative.WT_OK) {
                atlas.members -= animId
                anim.atlas = null
                touched += atlas
            }
            if (anim.atlas == null && !anim.hasProducer) {
                // Back on a texture of its own; the native side redraws the
                // current frame into it.
                val producer = try {
                    registry.createSurfaceProducer(TextureRegistry.SurfaceLifecycle.resetInBackground)
                } catch (e: RuntimeException) {
                    throw FlutterError("ENGINE_DETACHED", e.message, null)
                }
                anim.attach(producer, anim.width, anim.height)
            }
            AnimationHandleMessage(animId, anim.textureId)
        }
        val retiredIds = ArrayList<Long>()
        val pages = ArrayList<AtlasPageMessage>()
        for (atlas in touched) {
            retiredIds += atlas.sync(registry)
            pages += atlas.pageMessages()
        }
        return AtlasLayoutMessage(emptyList(), handles, pages, retiredIds)
    }

    override fun releaseTextures(textureIds: List<Long>) {
        for (id in textureIds) retired.remove(id)?.release()
    }

    override fun atlasStats(atlasId: Long): AtlasStatsMessage {
        val atlas = atlases[atlasId]
            ?: throw FlutterError("WT_ERR_INVALID_ARGUMENT", "unknown atlas $atlasId", null)
        return AtlasStatsMessage(atlas.stats(), atlas.textureCount.toLong(), pluginTextureCount().toLong())
    }

    override fun disposeAtlas(atlasId: Long): AtlasLayoutMessage {
        val atlas = atlases.remove(atlasId)
            ?: return AtlasLayoutMessage(emptyList(), emptyList(), emptyList(), emptyList())
        for (animId in atlas.members) animations[animId]?.atlas = null
        return AtlasLayoutMessage(emptyList(), emptyList(), emptyList(), atlas.dispose())
    }

    /** Keeps [producer] (no longer drawn into) until Dart releases it. */
    private fun retire(producer: TextureRegistry.SurfaceProducer) {
        retired[producer.id()] = producer
        // A Dart side that never releases can't grow this without bound.
        while (retired.size > MAX_RETIRED) {
            val oldest = retired.keys.first()
            retired.remove(oldest)?.release()
        }
    }

    private fun pluginTextureCount(): Int =
        animations.values.count { it.hasProducer } + atlases.values.sumOf { it.textureCount } + retired.size

    override fun configureCache(directory: String?): String {
        val current = cacheDir ?: throw FlutterError("ENGINE_DETACHED", "the Flutter engine was detached", null)
        if (directory == null) return current.path
        val dir = File(directory)
        if (!dir.isAbsolute) {
            throw FlutterError("WT_ERR_INVALID_ARGUMENT", "the cache directory must be absolute: $directory", null)
        }
        cacheDir = dir
        return dir.path
    }

    override fun deviceSignals(): DeviceSignalsMessage =
        deviceSignals?.read()
            ?: throw FlutterError("ENGINE_DETACHED", "the Flutter engine was detached", null)

    override fun setSchedulerOptions(workerCount: Long, maxFps: Long, maxConcurrent: Long) {
        val status = if (workerCount !in 1..Int.MAX_VALUE || maxFps !in 1..Int.MAX_VALUE || maxConcurrent !in 0..Int.MAX_VALUE) {
            WtNative.WT_ERR_INVALID_ARGUMENT
        } else {
            WtNative.nativeSetSchedulerOptions(workerCount.toInt(), maxFps.toInt(), maxConcurrent.toInt())
        }
        if (status != WtNative.WT_OK) {
            throw FlutterError(WtNative.nativeStatusName(status), "can't apply the scheduler options", null)
        }
    }

    /** Any thread: forwards a native failure to Dart on [streamErrors]. */
    private fun postError(animId: Long, code: String, message: String) {
        main.post {
            if (animations.containsKey(animId)) {
                errorSink?.success(AnimationErrorMessage(animId, code, message))
            }
        }
    }

    override fun onMethodCall(
        call: MethodCall,
        result: Result
    ) {
        if (call.method == "getPlatformVersion") {
            result.success("Android ${android.os.Build.VERSION.RELEASE}")
        } else {
            result.notImplemented()
        }
    }

    override fun onDetachedFromEngine(binding: FlutterPlugin.FlutterPluginBinding) {
        channel.setMethodCallHandler(null)
        WebmTextureHostApi.setUp(binding.binaryMessenger, null)
        errorChannel?.setStreamHandler(null)
        errorChannel = null
        signalsChannel?.setStreamHandler(null)
        signalsChannel = null
        deviceSignals?.stop()
        deviceSignals = null
        errorSink = null
        atlases.values.forEach { it.dispose() }
        atlases.clear()
        animations.values.forEach { it.dispose() }
        animations.clear()
        retired.values.forEach { it.release() }
        retired.clear()
        textures = null
        cacheDir = null
    }
}
