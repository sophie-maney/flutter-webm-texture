package com.example.webm_texture

import android.app.ActivityManager
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.Build
import android.os.PowerManager

/**
 * Reads the inputs to tier detection (PLAN.md, "Tiers") and reports when
 * battery saver or the thermal status changes. The tier rules themselves live
 * in Dart (`StickerTiers`); this only reports facts.
 */
internal class DeviceSignals(private val context: Context) {
    private val power = context.getSystemService(Context.POWER_SERVICE) as PowerManager
    private var receiver: BroadcastReceiver? = null
    private var thermalListener: PowerManager.OnThermalStatusChangedListener? = null

    /** Any thread. */
    fun read(): DeviceSignalsMessage {
        val activity = context.getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager
        val memory = ActivityManager.MemoryInfo().also { activity.getMemoryInfo(it) }
        return DeviceSignalsMessage(
            ramMb = memory.totalMem / (1024 * 1024),
            cores = Runtime.getRuntime().availableProcessors().toLong(),
            lowRam = activity.isLowRamDevice,
            perfClass = perfClass(),
            lowPower = power.isPowerSaveMode,
            thermal = thermal(),
        )
    }

    private fun perfClass(): PerfClassMessage {
        // 0 when the device doesn't claim a class; otherwise the Android
        // version the device is certified for: 30 (R), 31 (S), 33+ (T and up).
        // The platform never reports a "low" class.
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) return PerfClassMessage.UNKNOWN
        return when (val pc = Build.VERSION.MEDIA_PERFORMANCE_CLASS) {
            0 -> PerfClassMessage.UNKNOWN
            in 1..31 -> PerfClassMessage.MID
            else -> if (pc >= Build.VERSION_CODES.TIRAMISU) PerfClassMessage.HIGH else PerfClassMessage.MID
        }
    }

    private fun thermal(): ThermalStateMessage {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) return ThermalStateMessage.UNKNOWN
        return when (power.currentThermalStatus) {
            PowerManager.THERMAL_STATUS_NONE -> ThermalStateMessage.NOMINAL
            PowerManager.THERMAL_STATUS_LIGHT, PowerManager.THERMAL_STATUS_MODERATE -> ThermalStateMessage.FAIR
            PowerManager.THERMAL_STATUS_SEVERE -> ThermalStateMessage.SERIOUS
            else -> ThermalStateMessage.CRITICAL
        }
    }

    /** Calls [onChange] on the main thread when battery saver or the thermal status changes. */
    fun listen(onChange: () -> Unit) {
        stop()
        val r = object : BroadcastReceiver() {
            override fun onReceive(context: Context?, intent: Intent?) = onChange()
        }
        // Delivered on the main thread, where the plugin's state lives.
        context.registerReceiver(r, IntentFilter(PowerManager.ACTION_POWER_SAVE_MODE_CHANGED))
        receiver = r
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            val l = PowerManager.OnThermalStatusChangedListener { onChange() }
            power.addThermalStatusListener(context.mainExecutor, l)
            thermalListener = l
        }
    }

    /** Stops listening. */
    fun stop() {
        receiver?.let { context.unregisterReceiver(it) }
        receiver = null
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            thermalListener?.let { power.removeThermalStatusListener(it) }
        }
        thermalListener = null
    }
}
