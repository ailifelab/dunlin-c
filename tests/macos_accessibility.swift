// Native acceptance utility; use only for the owned Dunlin C application.
// Allow actions require per-proposal operator review and task authorization.
// Never invoke allow automatically from a model/event callback.
import AppKit
import ApplicationServices
import Foundation

let args = CommandLine.arguments
guard let pid = pid_t(args[1]),
  NSRunningApplication(processIdentifier: pid)?.bundleIdentifier == "org.dunlin.desktop"
else { fatalError("Only owned Dunlin C application targets are supported") }
let app = AXUIElementCreateApplication(pid)
func attr(_ e: AXUIElement, _ key: String) -> Any? {
  var v: CFTypeRef?
  guard AXUIElementCopyAttributeValue(e, key as CFString, &v) == .success else { return nil }
  return v
}
func text(_ e: AXUIElement, _ key: String) -> String { attr(e, key) as? String ?? "" }
var nodes: [AXUIElement] = []
func scan(_ e: AXUIElement, _ depth: Int) {
  guard depth < 18, nodes.count < 800 else { return }
  nodes.append(e)
  for c in attr(e, kAXChildrenAttribute) as? [AXUIElement] ?? [] { scan(c, depth + 1) }
}
scan(app, 0)
let mode = args.count > 2 ? args[2] : "dump"
if mode == "dump" {
  for (i, e) in nodes.enumerated() {
    let role = text(e, kAXRoleAttribute)
    let sub = text(e, kAXSubroleAttribute)
    let value =
      sub.contains("Secure")
      ? "<redacted>" : String(describing: attr(e, kAXValueAttribute) ?? "").prefix(180)
    print(
      i, role, text(e, kAXIdentifierAttribute), text(e, kAXTitleAttribute),
      text(e, kAXDescriptionAttribute), value)
  }
} else if mode == "front" {
  print(
    AXUIElementSetAttributeValue(app, kAXFrontmostAttribute as CFString, kCFBooleanTrue).rawValue)
} else {
  let name = args[3]
  guard
    var e =
      (name.hasPrefix("#")
        ? Int(name.dropFirst()).flatMap { nodes.indices.contains($0) ? nodes[$0] : nil } : nil)
      ?? nodes.first(where: {
        text($0, kAXIdentifierAttribute) == name || text($0, kAXTitleAttribute) == name
          || text($0, kAXDescriptionAttribute) == name || text($0, kAXValueAttribute) == name
      })
  else {
    print("not found", name)
    exit(2)
  }
  if mode == "type" {
    _ = AXUIElementSetAttributeValue(app, kAXFrontmostAttribute as CFString, kCFBooleanTrue)
    _ = AXUIElementSetAttributeValue(e, kAXFocusedAttribute as CFString, kCFBooleanTrue)
    RunLoop.current.run(until: Date().addingTimeInterval(0.1))
    if let pv = attr(e, kAXPositionAttribute), let sv = attr(e, kAXSizeAttribute) {
      var point = CGPoint.zero
      var size = CGSize.zero
      AXValueGetValue(pv as! AXValue, .cgPoint, &point)
      AXValueGetValue(sv as! AXValue, .cgSize, &size)
      point.x += size.width / 2
      point.y += size.height / 2
      for type in [CGEventType.leftMouseDown, .leftMouseUp] {
        CGEvent(
          mouseEventSource: nil, mouseType: type, mouseCursorPosition: point, mouseButton: .left)?
          .post(tap: .cghidEventTap)
      }
    }
    RunLoop.current.run(until: Date().addingTimeInterval(0.1))
    for down in [true, false] {
      let ev = CGEvent(keyboardEventSource: nil, virtualKey: 0, keyDown: down)!
      ev.flags = .maskCommand
      ev.post(tap: .cghidEventTap)
    }
    let chars = Array(args[4].utf16)
    for down in [true, false] {
      let ev = CGEvent(keyboardEventSource: nil, virtualKey: 0, keyDown: down)!
      chars.withUnsafeBufferPointer {
        ev.keyboardSetUnicodeString(stringLength: chars.count, unicodeString: $0.baseAddress!)
      }
      ev.post(tap: .cghidEventTap)
    }
  }
  if mode == "nonempty" {
    let value = text(e, kAXValueAttribute)
    print("nonempty", !value.isEmpty)
    exit(value.isEmpty ? 3 : 0)
  }
  if mode == "select" {
    for _ in 0..<8 {
      if text(e, kAXRoleAttribute) == "AXRow" { break }
      guard let parent = attr(e, kAXParentAttribute) else { break }
      e = parent as! AXUIElement
    }
    print(
      AXUIElementSetAttributeValue(e, kAXSelectedAttribute as CFString, kCFBooleanTrue).rawValue)
  }
  if mode == "close" {
    if let button = attr(e, kAXCloseButtonAttribute) {
      print(AXUIElementPerformAction(button as! AXUIElement, kAXPressAction as CFString).rawValue)
    }
  }
  if mode == "press" {
    let result = AXUIElementPerformAction(e, kAXPressAction as CFString)
    print(result.rawValue)
    exit(result == .success ? 0 : 3)
  }
  if mode == "set" {
    let result = AXUIElementSetAttributeValue(e, kAXValueAttribute as CFString, args[4] as CFString)
    print(result.rawValue)
    exit(result == .success ? 0 : 3)
  }
  if mode == "focus" {
    let result = AXUIElementSetAttributeValue(e, kAXFocusedAttribute as CFString, kCFBooleanTrue)
    print(result.rawValue)
  }
}
