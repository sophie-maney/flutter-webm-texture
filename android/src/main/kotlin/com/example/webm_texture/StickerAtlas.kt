package com.example.webm_texture

import android.util.Log
import io.flutter.view.TextureRegistry

private const val TAG = "WebmTexture"

/**
 * One native atlas (`wt_atlas`, T4.2, [ADR 0020](docs/decisions/0020-atlas-compositor.md)):
 * pages of animations, each page one `SurfaceProducer` that the native
 * scheduler presents into once per tick.
 *
 * A page's buffers have a fixed size, so when the core wants a page taller
 * or shorter (`wt_atlas_page_info.wantedHeight`), [sync] gives it a new
 * producer of that height and hands the old one to [retire]: it keeps
 * showing its last frame until Dart, which still lays slots out at the old
 * size until it gets the new texture id, releases it.
 *
 * Every method runs on the platform main thread.
 */
internal class StickerAtlas(
    val atlasId: Long,
    private val handle: Long,
    private val retire: (TextureRegistry.SurfaceProducer) -> Unit,
) {
    /** Animations in the atlas, by id. */
    val members = HashSet<Long>()

    /** The page's current producer and the height it was created with. */
    private inner class PageSurface(
        val page: Int,
        val producer: TextureRegistry.SurfaceProducer,
        val width: Int,
        val height: Int,
    ) : TextureRegistry.SurfaceProducer.Callback {
        override fun onSurfaceAvailable() {
            // A new surface after backgrounding: the core presents the whole
            // page into it from the slots it keeps.
            val status = WtNative.nativeAtlasSetSurface(handle, page, producer.surface, height)
            if (status != WtNative.WT_OK) {
                Log.w(TAG, "atlas $atlasId page $page: surface: ${WtNative.nativeStatusName(status)}")
            }
        }

        override fun onSurfaceCleanup() {
            // The surface dies after this returns: detach it synchronously.
            WtNative.nativeAtlasSetSurface(handle, page, null, height)
        }
    }

    private val pages = HashMap<Int, PageSurface>()

    /** Moves [anim] into a slot; returns its `wt_status` and fills [out] (`SLOT_*`). */
    fun attach(anim: StickerTexture, out: IntArray): Int =
        WtNative.nativeAtlasAttach(handle, anim.nativeHandle, out)

    /** Moves [anim] back to its own (possibly absent) surface; returns a `wt_status`. */
    fun detach(anim: StickerTexture): Int = WtNative.nativeAtlasDetach(handle, anim.nativeHandle)

    /**
     * Gives every page a producer of the height it wants now, or none.
     * Returns the texture ids of the producers it retired.
     */
    fun sync(textures: TextureRegistry): List<Long> {
        val retired = ArrayList<Long>()
        val info = LongArray(WtNative.PAGE_LENGTH)
        for (page in 0 until WtNative.nativeAtlasPageCount(handle)) {
            if (WtNative.nativeAtlasPageInfo(handle, page, info) != WtNative.WT_OK) continue
            val wanted = info[WtNative.PAGE_WANTED_HEIGHT].toInt()
            val width = info[WtNative.PAGE_WIDTH].toInt()
            val current = pages[page]
            if ((current?.height ?: 0) == wanted) continue
            var next: PageSurface? = null
            if (wanted > 0) {
                val producer = try {
                    textures.createSurfaceProducer(TextureRegistry.SurfaceLifecycle.resetInBackground)
                } catch (e: RuntimeException) {
                    // FlutterJNI detached (spike B): keep what the page has.
                    Log.w(TAG, "atlas $atlasId page $page: no producer: ${e.message}")
                    continue
                }
                producer.setSize(width, wanted)
                val status = WtNative.nativeAtlasSetSurface(handle, page, producer.surface, wanted)
                if (status != WtNative.WT_OK) {
                    Log.w(TAG, "atlas $atlasId page $page: surface: ${WtNative.nativeStatusName(status)}")
                    producer.release()
                    continue
                }
                next = PageSurface(page, producer, width, wanted)
                producer.setCallback(next)
                Log.i(TAG, "atlas $atlasId page $page: ${producer.javaClass.simpleName} id=${producer.id()} ${width}x$wanted")
            } else {
                WtNative.nativeAtlasSetSurface(handle, page, null, 0)
            }
            if (current != null) {
                current.producer.setCallback(null)
                retired += current.producer.id()
                retire(current.producer)
            }
            if (next != null) pages[page] = next else pages.remove(page)
        }
        return retired
    }

    /** The texture id that shows [anim]'s slot's frame, or -1 (`wt_atlas_shown_page`). */
    fun shownTexture(anim: StickerTexture): Long {
        val page = WtNative.nativeAtlasShownPage(handle, anim.nativeHandle)
        return if (page < 0) -1 else pages[page]?.producer?.id() ?: -1
    }

    /** Every page that has a texture. */
    fun pageMessages(): List<AtlasPageMessage> =
        pages.values.sortedBy { it.page }.map {
            AtlasPageMessage(atlasId, it.page.toLong(), it.producer.id(), it.width.toLong(), it.height.toLong())
        }

    val textureCount: Int get() = pages.size

    fun stats(): List<AtlasPageStatsMessage> {
        val info = LongArray(WtNative.PAGE_LENGTH)
        val out = ArrayList<AtlasPageStatsMessage>()
        for (page in 0 until WtNative.nativeAtlasPageCount(handle)) {
            if (WtNative.nativeAtlasPageInfo(handle, page, info) != WtNative.WT_OK) continue
            val surface = pages[page]
            out += AtlasPageStatsMessage(
                pageId = page.toLong(),
                textureId = surface?.producer?.id() ?: -1,
                width = info[WtNative.PAGE_WIDTH],
                height = (surface?.height ?: 0).toLong(),
                usedHeight = info[WtNative.PAGE_USED_HEIGHT],
                liveSlots = info[WtNative.PAGE_LIVE_SLOTS],
                buffers = info[WtNative.PAGE_BUFFERS],
                bufferBytes = info[WtNative.PAGE_BUFFER_BYTES],
                cpuBytes = info[WtNative.PAGE_CPU_BYTES],
                presents = info[WtNative.PAGE_PRESENTS],
                presentFailures = info[WtNative.PAGE_FAILURES],
            )
        }
        return out
    }

    /**
     * Destroys the native atlas (its animations go back to their own, absent,
     * surfaces) and retires every page producer. Returns their texture ids.
     */
    fun dispose(): List<Long> {
        WtNative.nativeAtlasDestroy(handle)
        val retired = ArrayList<Long>()
        for (surface in pages.values) {
            surface.producer.setCallback(null)
            retired += surface.producer.id()
            retire(surface.producer)
        }
        pages.clear()
        members.clear()
        return retired
    }
}
