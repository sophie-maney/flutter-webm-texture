// Screenshot set-up shared by the integration tests that read pixels back.
import 'dart:io';

import 'package:integration_test/integration_test.dart';

var _prepared = false;

/// Makes [IntegrationTestWidgetsFlutterBinding.takeScreenshot] work. Android
/// draws Flutter into a surface that can't be read back until it is
/// converted to an image, which may happen only once per app launch, so
/// every test file that takes screenshots calls this instead.
Future<void> prepareScreenshots(
  IntegrationTestWidgetsFlutterBinding binding,
) async {
  if (_prepared || !Platform.isAndroid) return;
  _prepared = true;
  await binding.convertFlutterSurfaceToImage();
}
