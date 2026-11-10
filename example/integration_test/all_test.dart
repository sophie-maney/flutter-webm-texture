// Every integration test in one app launch. CI runs this file on the iOS
// simulator, where each separate test file costs a full Xcode build and app
// launch (and a chance of the simulator failing to start the app). Run one
// file on its own while developing:
//
//   cd example && fvm flutter test integration_test/app_test.dart -d <device>
import 'package:integration_test/integration_test.dart';

import 'app_test.dart' as app;
import 'plugin_integration_test.dart' as plugin;
import 'probe_test.dart' as probe;
import 'texture_test.dart' as texture;
import 'visibility_test.dart' as visibility;

void main() {
  IntegrationTestWidgetsFlutterBinding.ensureInitialized();
  app.main();
  plugin.main();
  probe.main();
  texture.main();
  visibility.main();
}
