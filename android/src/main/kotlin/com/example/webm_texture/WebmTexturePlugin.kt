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
        animations.remove(animId)?.dispose()
    }

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
        animations.values.forEach { it.dispose() }
        animations.clear()
        textures = null
        cacheDir = null
    }
}
