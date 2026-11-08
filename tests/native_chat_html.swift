import AppKit
import SwiftUI
import WebKit
import Darwin

private struct Failure: Error, CustomStringConvertible { let description: String }
@MainActor private func check(_ value: Bool, _ message: String) throws {
  if !value { throw Failure(description: message) }
}
@MainActor private func waitFor(_ message: String, _ predicate: () -> Bool) async throws {
  let end = Date().addingTimeInterval(20)
  while !predicate() {
    if Date() > end { throw Failure(description: "timed out: " + message) }
    try await Task.sleep(for: .milliseconds(20))
  }
}
@MainActor private func webView(in view: NSView) -> WKWebView? {
  if let web = view as? WKWebView { return web }
  return view.subviews.lazy.compactMap { webView(in: $0) }.first
}
@MainActor private func evaluate(_ web: WKWebView, _ script: String) async throws -> Any {
  try await withCheckedThrowingContinuation { continuation in
    web.evaluateJavaScript(script, in: nil, in: .defaultClient) { continuation.resume(with: $0) }
  }
}
@main private struct NativeHTMLChecks {
  @MainActor static func main() {
    NSApplication.shared.setActivationPolicy(.prohibited)
    Task { @MainActor in
      do { try await run(); print("PASS actual hosted ChatHTMLContent/WebKit: disabled page scripts, ephemeral store, escaped elements, no automatic remote resources, current source, theme and resize/stream heights"); exit(0) }
      catch { print("FAIL \(error)"); exit(1) }
    }
    NSApplication.shared.run()
  }
  @MainActor static func run() async throws {
    let args = ProcessInfo.processInfo.arguments
    guard let i = args.firstIndex(of: "--fixture-endpoint"), i + 1 < args.count,
      args[i+1].hasPrefix("http://127.0.0.1:") else { throw Failure(description: "owned loopback peer required") }
    let endpoint = args[i+1]
    let hostile = "<script>window.__modelRan = true; fetch('\(endpoint)/script')</script>\n<img src='\(endpoint)/pixel'><iframe src='\(endpoint)/frame'></iframe>\n\n[可点击的链接](\(endpoint)/explicit-link)"
    let text = "# 原始长回复\n\n" + String(repeating: "这一段说明用于检验换行与尺寸。Please preserve the actual source. ", count: 25) + "\n\n" + hostile
    func content(_ source: String, dark: Bool = false) -> AnyView {
      AnyView(ChatHTMLContent(source: source).environment(\.chatBodySize, 16).environment(\.colorScheme, dark ? .dark : .light))
    }
    let host = NSHostingView(rootView: content(text))
    let window = NSWindow(contentRect: NSRect(x: 0,y: 0,width: 900,height: 1400), styleMask: .borderless, backing: .buffered, defer: false)
    window.contentView = host
    host.layoutSubtreeIfNeeded()
    try await waitFor("real WebKit view") { webView(in: host) != nil }
    let web = webView(in: host)!
    try await waitFor("initial HTML load") { !web.isLoading && web.url?.absoluteString == "about:blank" }
    let ready = try await evaluate(web, "document.querySelector('article') !== null")
    try check((ready as? Bool) == true, "document not loaded")
    try check(!web.configuration.defaultWebpagePreferences.allowsContentJavaScript, "page JavaScript enabled")
    try check(!web.configuration.websiteDataStore.isPersistent, "persistent web store")
    try check(web.navigationDelegate != nil, "missing navigation gate")
    let elements = try await evaluate(web, "document.querySelectorAll('script,img,iframe,object,embed').length")
    try check((elements as? NSNumber)?.intValue == 0, "model HTML became an executable/resource element")
    let ran = try await evaluate(web, "typeof window.__modelRan")
    try check((ran as? String) == "undefined", "model script executed")
    let wide = (try await evaluate(web, "document.querySelector('article').getBoundingClientRect().height") as! NSNumber).doubleValue
    print("wide target=\(wide), initial native frame=\(web.frame)")
    do {
      try await waitFor("wide auto height") { host.layoutSubtreeIfNeeded();return abs(web.frame.height - ceil(wide)) <= 2 }
    } catch {
      let coordinator = web.navigationDelegate as? ChatHTMLView.Coordinator
      let current = try await evaluate(web, "({height:document.querySelector('article').getBoundingClientRect().height,width:window.innerWidth})")
      print("layout diagnostic: binding=\(coordinator?.height.wrappedValue ?? -1), web=\(web.frame), host=\(host.frame), DOM=\(current)")
      throw error
    }
    window.setContentSize(NSSize(width: 480,height: 1400));host.layoutSubtreeIfNeeded()
    try await waitFor("narrow web width") { web.frame.width < 500 }
    let narrow = (try await evaluate(web, "document.querySelector('article').getBoundingClientRect().height") as! NSNumber).doubleValue
    try check(narrow > wide, "resize did not wrap content")
    try await waitFor("resize auto height") { host.layoutSubtreeIfNeeded();return abs(web.frame.height - ceil(narrow)) <= 2 }
    // Actual SwiftUI updates race old WebKit completion. The visible and measured
    // result must correspond to the final source and theme, not the long draft.
    host.rootView = content("# 临时流式内容\n\n" + String(repeating: "temporary ",count: 100))
    host.rootView = content("最终回复 **完成**。", dark: true)
    host.layoutSubtreeIfNeeded()
    try await Task.sleep(for: .milliseconds(150))
    try await waitFor("final document load") { !web.isLoading }
    let final = try await evaluate(web, "document.querySelector('article').textContent")
    try check((final as? String) == "最终回复 完成。", "stale source remained visible")
    let style = try await evaluate(web, "getComputedStyle(document.documentElement).colorScheme")
    try check((style as? String) == "dark", "theme not applied")
    let finalHeight = (try await evaluate(web, "document.querySelector('article').getBoundingClientRect().height") as! NSNumber).doubleValue
    try await waitFor("final short height") { host.layoutSubtreeIfNeeded();return abs(web.frame.height - max(24,ceil(finalHeight))) <= 2 }
    try check(web.frame.height < narrow, "old source height survived final update")
    window.contentView = nil
    window.close()
  }
}
