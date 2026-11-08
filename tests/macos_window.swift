import AppKit
import CoreGraphics
import Foundation

let pid = Int(CommandLine.arguments[1])!
guard NSRunningApplication(processIdentifier: pid_t(pid))?.bundleIdentifier == "org.dunlin.desktop"
else { fatalError("Only Dunlin C windows") }
let windows =
  CGWindowListCopyWindowInfo([.optionOnScreenOnly, .excludeDesktopElements], kCGNullWindowID)
  as? [[String: Any]] ?? []
for w in windows where w[kCGWindowOwnerPID as String] as? Int == pid {
  print(w[kCGWindowNumber as String] ?? "unknown")
}
