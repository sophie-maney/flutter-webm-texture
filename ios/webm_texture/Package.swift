// swift-tools-version: 5.9
// The swift-tools-version declares the minimum version of Swift required to build this package.

import PackageDescription

let package = Package(
    name: "webm_texture",
    platforms: [
        .iOS("15.0")
    ],
    products: [
        .library(name: "webm-texture", targets: ["webm_texture"])
    ],
    dependencies: [
        .package(name: "FlutterFramework", path: "../FlutterFramework")
    ],
    targets: [
        // Prebuilt libvpx, libwebm, libyuv, libwebp, giflib and lz4, merged into
        // one static library. Filled by `fvm dart run tool/fetch_prebuilts.dart`
        // (Frameworks/ is a symlink into third_party/prebuilt, which is gitignored).
        .binaryTarget(
            name: "WebmTextureDeps",
            path: "Frameworks/WebmTextureDeps.xcframework"
        ),
        // The C++ core. Sources/wt_core holds symlinks to ../../../src (SwiftPM
        // can't reach outside the package); include/ exposes only the C ABI.
        .target(
            name: "wt_core",
            dependencies: ["WebmTextureDeps"],
            path: "Sources/wt_core",
            publicHeadersPath: "include",
            cxxSettings: [
                .headerSearchPath("."),
                .unsafeFlags(["-fno-exceptions", "-fno-rtti"]),
            ]
        ),
        .target(
            name: "webm_texture",
            dependencies: [
                .product(name: "FlutterFramework", package: "FlutterFramework"),
                "wt_core",
            ],
            resources: [
                // If your plugin requires a privacy manifest, for example if it uses any required
                // reason APIs, update the PrivacyInfo.xcprivacy file to describe your plugin's
                // privacy impact, and then uncomment these lines. For more information, see
                // https://developer.apple.com/documentation/bundleresources/privacy_manifest_files
                // .process("PrivacyInfo.xcprivacy"),

                // If you have other resources that need to be bundled with your plugin, refer to
                // the following instructions to add them:
                // https://developer.apple.com/documentation/xcode/bundling-resources-with-a-swift-package
            ]
        )
    ],
    cxxLanguageStandard: .cxx17
)
