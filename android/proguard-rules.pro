# Consumer R8 rules: JNI looks these up by name (src/main/cpp/jni_bridge.cc).
-keep class com.example.webm_texture.WtNative {
    native <methods>;
}
-keep interface com.example.webm_texture.NativeErrorListener { *; }
-keepclassmembers class * implements com.example.webm_texture.NativeErrorListener {
    void onNativeError(java.lang.String, java.lang.String);
}
