package com.example.webm_texture

import io.flutter.embedding.engine.plugins.FlutterPlugin
import io.flutter.plugin.common.MethodCall
import io.flutter.plugin.common.MethodChannel
import io.flutter.plugin.common.MethodChannel.MethodCallHandler
import io.flutter.plugin.common.MethodChannel.Result

/** WebmTexturePlugin */
class WebmTexturePlugin :
    FlutterPlugin,
    MethodCallHandler,
    WebmTextureHostApi {
    // The MethodChannel that will the communication between Flutter and native Android
    //
    // This local reference serves to register the plugin with the Flutter Engine and unregister it
    // when the Flutter Engine is detached from the Activity
    private lateinit var channel: MethodChannel

    override fun onAttachedToEngine(flutterPluginBinding: FlutterPlugin.FlutterPluginBinding) {
        channel = MethodChannel(flutterPluginBinding.binaryMessenger, "webm_texture")
        channel.setMethodCallHandler(this)
        WebmTextureHostApi.setUp(flutterPluginBinding.binaryMessenger, this)
    }

    // T2.2 replaces these stubs with the SurfaceProducer glue.
    override suspend fun createAnimation(
        sourceKey: String,
        source: AnimationSourceMessage,
        bucketW: Long,
        bucketH: Long
    ): AnimationHandleMessage = throw notImplemented("createAnimation")

    override fun setActive(animId: Long, active: Boolean) = throw notImplemented("setActive")

    override fun dispose(animId: Long) = throw notImplemented("dispose")

    // T3.4 reports the real device signals.
    override fun deviceSignals(): DeviceSignalsMessage = throw notImplemented("deviceSignals")

    private fun notImplemented(method: String) =
        FlutterError("UNIMPLEMENTED", "$method is not implemented on Android yet", null)

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
    }
}
