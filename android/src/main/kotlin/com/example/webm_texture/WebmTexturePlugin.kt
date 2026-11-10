package com.example.webm_texture

import android.os.Handler
import android.os.Looper
import io.flutter.embedding.engine.plugins.FlutterPlugin
import io.flutter.plugin.common.MethodCall
import io.flutter.plugin.common.MethodChannel
import io.flutter.plugin.common.MethodChannel.MethodCallHandler
import io.flutter.plugin.common.MethodChannel.Result
import io.flutter.view.TextureRegistry
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/**
 * The Android side of the Pigeon API ([ADR 0010](docs/decisions/0010-pigeon-api-shape.md)).
 * Each animation is a [StickerTexture]: a `SurfaceProducer` filled by a native
 * render thread. Host API calls arrive on the platform main thread.
 */
class WebmTexturePlugin :
    FlutterPlugin,
    MethodCallHandler,
    WebmTextureHostApi {
    private lateinit var channel: MethodChannel
    private var errorChannel: StreamErrorsEventChannel? = null
    private var textures: TextureRegistry? = null
    private val main = Handler(Looper.getMainLooper())

    // Main thread only.
    private val animations = HashMap<Long, StickerTexture>()
    private var nextAnimId = 1L
    private var errorSink: PigeonEventSink<AnimationErrorMessage>? = null

    override fun onAttachedToEngine(flutterPluginBinding: FlutterPlugin.FlutterPluginBinding) {
        textures = flutterPluginBinding.textureRegistry
        channel = MethodChannel(flutterPluginBinding.binaryMessenger, "webm_texture")
        channel.setMethodCallHandler(this)
        WebmTextureHostApi.setUp(flutterPluginBinding.binaryMessenger, this)
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
        val animId = nextAnimId++
        val texture = StickerTexture(animId, ::postError)

        // Probing and decoder setup read the file, so keep them off main.
        val out = withContext(Dispatchers.IO) {
            texture.createNative(source, bucketW.toInt(), bucketH.toInt(), loop)
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

    // T3.4 reports the real device signals.
    override fun deviceSignals(): DeviceSignalsMessage =
        throw FlutterError("UNIMPLEMENTED", "deviceSignals is not implemented on Android yet", null)

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
        errorSink = null
        animations.values.forEach { it.dispose() }
        animations.clear()
        textures = null
    }
}
