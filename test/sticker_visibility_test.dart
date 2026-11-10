import 'package:flutter/widgets.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:webm_texture/src/sticker_visibility.dart';

void main() {
  Widget app(Widget child) =>
      Directionality(textDirection: TextDirection.ltr, child: child);

  Rect? rectOf(WidgetTester tester, Key key) =>
      RenderStickerVisibility.visibleRect(
        tester.renderObject<RenderStickerVisibility>(
          find.byKey(key, skipOffstage: false),
        ),
      );

  Widget probe(Key key, List<bool> log, {double size = 100}) =>
      StickerVisibility(
        key: key,
        onChanged: log.add,
        child: SizedBox(width: size, height: size),
      );

  testWidgets('reports the on-screen part, and only changes', (tester) async {
    const key = Key('p');
    final log = <bool>[];
    Widget build(double left) => app(
      Stack(
        children: [Positioned(left: left, top: 0, child: probe(key, log))],
      ),
    );
    await tester.pumpWidget(build(750)); // test view is 800 x 600
    expect(rectOf(tester, key), const Rect.fromLTWH(0, 0, 50, 100));
    expect(log, [true]);

    await tester.pumpWidget(build(700));
    await tester.pumpWidget(build(760));
    expect(log, [true], reason: 'still visible: no new report');

    await tester.pumpWidget(build(800));
    expect(rectOf(tester, key), isNull, reason: 'touching the edge');
    expect(log, [true, false]);

    await tester.pumpWidget(const SizedBox());
    expect(log, [true, false], reason: 'already hidden');
  });

  testWidgets('detaching reports hidden', (tester) async {
    final log = <bool>[];
    await tester.pumpWidget(app(Center(child: probe(const Key('p'), log))));
    await tester.pumpWidget(const SizedBox());
    expect(log, [true, false]);
  });

  testWidgets('follows transforms', (tester) async {
    const key = Key('p');
    final log = <bool>[];
    await tester.pumpWidget(
      app(
        Align(
          alignment: Alignment.topLeft,
          child: Transform.translate(
            offset: const Offset(-75, 0),
            child: Transform.scale(
              scale: 2,
              alignment: Alignment.topLeft,
              child: probe(key, log),
            ),
          ),
        ),
      ),
    );
    // 200 px wide after scaling, 75 of them off to the left; in local
    // (unscaled) units that leaves x in [37.5, 100].
    final r = rectOf(tester, key)!;
    expect(r.left, moreOrLessEquals(37.5));
    expect(r.right, moreOrLessEquals(100));
    expect(log, [true]);
  });

  testWidgets('a horizontal list inside a vertical one', (tester) async {
    final logs = List.generate(10, (_) => <bool>[]);
    await tester.pumpWidget(
      app(
        ListView(
          children: [
            SizedBox(
              height: 100,
              child: ListView(
                scrollDirection: Axis.horizontal,
                children: [
                  for (var i = 0; i < 10; i++)
                    probe(Key('h$i'), logs[i], size: 300),
                ],
              ),
            ),
          ],
        ),
      ),
    );
    // 800 px wide: items 0-2 show (item 2 partly). Item 3 is built for the
    // cache extent but never shows, so it never reports.
    expect(
      [for (final l in logs) l.lastOrNull],
      [true, true, true, null, null, null, null, null, null, null],
    );
    expect(
      tester.renderObject(find.byKey(const Key('h3'), skipOffstage: false)),
      isA<RenderStickerVisibility>().having((r) => r.visible, 'visible', false),
    );
    expect(rectOf(tester, const Key('h2'))!.width, 200);
  });

  testWidgets('a kept-alive item scrolled away is hidden', (tester) async {
    const key = Key('kept');
    final log = <bool>[];
    final controller = ScrollController();
    await tester.pumpWidget(
      app(
        ListView.builder(
          controller: controller,
          itemCount: 100,
          itemExtent: 100,
          itemBuilder: (_, i) => i == 0
              ? _KeepAlive(child: probe(key, log))
              : const SizedBox(height: 100),
        ),
      ),
    );
    expect(log, [true]);
    controller.jumpTo(5000);
    await tester.pump();
    await tester.pump();
    // Still attached (kept alive) but not painted.
    expect(find.byKey(key, skipOffstage: false), findsOneWidget);
    expect(log, [true, false]);
    expect(rectOf(tester, key), isNull);
  });
}

class _KeepAlive extends StatefulWidget {
  const _KeepAlive({required this.child});

  final Widget child;

  @override
  State<_KeepAlive> createState() => _KeepAliveState();
}

class _KeepAliveState extends State<_KeepAlive>
    with AutomaticKeepAliveClientMixin {
  @override
  bool get wantKeepAlive => true;

  @override
  Widget build(BuildContext context) {
    super.build(context);
    return widget.child;
  }
}
