// T2.5: in a 200-item list of WebmStickers, only the rows on screen report
// active, as counted by the page's debug overlay. Run with:
//
//   cd example && fvm flutter test integration_test/visibility_test.dart -d <device>
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';
import 'package:webm_texture/webm_texture.dart';
import 'package:webm_texture_example/visibility_demo.dart';

void main() {
  IntegrationTestWidgetsFlutterBinding.ensureInitialized();

  /// Stickers whose box overlaps the list's viewport.
  int onScreen(WidgetTester tester) {
    final viewport = tester.getRect(find.byKey(const Key('stickerList')));
    var n = 0;
    for (final e in find.byType(WebmSticker, skipOffstage: false).evaluate()) {
      final box = e.renderObject! as RenderBox;
      final r = box.localToGlobal(Offset.zero) & box.size;
      final overlap = r.intersect(viewport);
      if (overlap.width > 0 && overlap.height > 0) n++;
    }
    return n;
  }

  int overlayCount(WidgetTester tester) {
    final text = tester.widget<Text>(find.byKey(const Key('activeCount')));
    return int.parse(text.data!.split(' ').last);
  }

  /// Pumps until every on-screen sticker has its texture.
  Future<void> waitForTextures(WidgetTester tester) async {
    // The page loads its assets before building the list.
    for (var i = 0; i < 200; i++) {
      if (find.byKey(const Key('stickerList')).evaluate().isNotEmpty) break;
      await tester.pump(const Duration(milliseconds: 50));
    }
    for (var i = 0; i < 200; i++) {
      await tester.pump(const Duration(milliseconds: 50));
      if (find.byType(Texture).evaluate().length >= onScreen(tester)) break;
    }
    // One more frame for the post-frame visibility check.
    await tester.pump(const Duration(milliseconds: 50));
  }

  Future<void> check(WidgetTester tester, String where) async {
    await waitForTextures(tester);
    final visible = onScreen(tester);
    final built = find.byType(WebmSticker, skipOffstage: false).evaluate();
    // ignore: avoid_print
    print(
      '$where: on screen $visible, built ${built.length}, '
      'overlay ${overlayCount(tester)}',
    );
    expect(visible, greaterThan(0), reason: where);
    expect(built.length, greaterThan(visible), reason: '$where: cache extent');
    expect(overlayCount(tester), visible, reason: where);
    expect(WebmStickers.debugActiveCount.value, visible, reason: where);
  }

  testWidgets('only on-screen stickers report active', (tester) async {
    await tester.pumpWidget(const MaterialApp(home: VisibilityDemoPage()));
    await check(tester, 'top');

    final list = find.byKey(const Key('stickerList'));
    await tester.drag(list, const Offset(0, -1000));
    await tester.pumpAndSettle();
    await check(tester, 'after a drag');

    // Leave a row half visible at the top edge.
    await tester.drag(list, const Offset(0, -48));
    await tester.pumpAndSettle();
    await check(tester, 'half a row');

    final scrollable = tester.state<ScrollableState>(
      find.descendant(of: list, matching: find.byType(Scrollable)),
    );
    scrollable.position.jumpTo(scrollable.position.maxScrollExtent);
    await tester.pumpAndSettle();
    await check(tester, 'bottom');

    // Leaving the page stops everything.
    await tester.pumpWidget(const MaterialApp(home: SizedBox()));
    await tester.pump();
    expect(WebmStickers.debugActiveCount.value, 0);
  });
}
