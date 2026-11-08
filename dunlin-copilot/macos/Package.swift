// swift-tools-version: 6.0
import PackageDescription
import Foundation
let root = URL(fileURLWithPath: #filePath).resolvingSymlinksInPath().deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent().path
let package = Package(name: "DunlinC", platforms: [.macOS("26.0")], targets: [
 .systemLibrary(name: "CDesktop", path: "Sources/CDesktop"),
 .executableTarget(name: "DunlinApp", dependencies: ["CDesktop"], linkerSettings: [.unsafeFlags(["-L",root+"/build/desktop/native-26-source",root+"/build/native-deps/macos26/lib/libjson-c.a",root+"/build/native-deps/macos26/lib/libcrypto.a"]), .linkedLibrary("dunlin-desktop"), .linkedLibrary("dunlin"), .linkedLibrary("dunlin-kernel"), .unsafeFlags([root+"/build/native-deps/media-current/lib/libcurl.a",root+"/build/native-deps/macos26/lib/libssl.a",root+"/build/native-deps/macos26/lib/libcrypto.a"]), .linkedFramework("SystemConfiguration"), .linkedFramework("Security"), .linkedFramework("CoreFoundation"), .linkedFramework("CoreServices"), .linkedLibrary("sqlite3"),.linkedLibrary("z")])], swiftLanguageModes: [.v5])
