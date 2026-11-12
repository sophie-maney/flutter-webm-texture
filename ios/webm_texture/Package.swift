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
        // ObjC++ shim over wt_api.h that decodes into CVPixelBuffers (T2.3).
        // Swift can't mix with ObjC in one target, so it is its own target.
        .target(
            name: "wt_ios",
            dependencies: ["wt_core"],
            path: "Sources/wt_ios",
            publicHeadersPath: "include",
            cxxSettings: [
                .unsafeFlags(["-fno-exceptions", "-fno-rtti"]),
            ],
            linkerSettings: [
                .linkedFramework("CoreVideo"),
                .linkedFramework("IOSurface"),
            ]
        ),
        .target(
            name: "webm_texture",
            dependencies: [
                .product(name: "FlutterFramework", package: "FlutterFramework"),
                "wt_core",
                "wt_ios",
            ],
            resources: [
                .process("PrivacyInfo.xcprivacy"),
            ]
        )
    ],
    cxxLanguageStandard: .cxx17
)
