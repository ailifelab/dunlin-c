import AppKit
import Foundation
// Native application handoff. Credential reference is passed in process environment,
// never in arguments, a configuration file or output.
let configuration=NSWorkspace.OpenConfiguration()
configuration.activates=true
if let key=ProcessInfo.processInfo.environment["OMLX_API_KEY"],!key.isEmpty {configuration.environment=["OMLX_API_KEY":key]}
NSWorkspace.shared.openApplication(at:URL(fileURLWithPath:CommandLine.arguments[1]),configuration:configuration){app,error in
 if let app {print("launched Dunlin C pid \(app.processIdentifier)");exit(0)}
 print("launch failed: \(error?.localizedDescription ?? "unknown")");exit(1)
}
RunLoop.main.run()
