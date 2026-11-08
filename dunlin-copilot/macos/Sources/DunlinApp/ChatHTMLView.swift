import AppKit
import SwiftUI
import WebKit

struct ChatHTMLView: NSViewRepresentable {
  let document: ChatHTMLDocument
  @Binding var height: CGFloat
  func makeCoordinator() -> Coordinator { Coordinator(height: $height) }
  func makeNSView(context: Context) -> WKWebView {
    let config = WKWebViewConfiguration()
    config.websiteDataStore = .nonPersistent()
    config.defaultWebpagePreferences.allowsContentJavaScript = false
    let view = ChatRenderingWebView(frame: .zero, configuration: config)
    view.onResize = { [weak coordinator = context.coordinator] in coordinator?.measure() }
    view.underPageBackgroundColor = .clear
    view.navigationDelegate = context.coordinator
    view.setAccessibilityLabel("AI 回复")
    context.coordinator.webView = view
    return view
  }
  func updateNSView(_ view: WKWebView, context: Context) {
    context.coordinator.height = $height
    guard context.coordinator.html != document.html else {
      context.coordinator.measure()
      return
    }
    context.coordinator.html = document.html
    context.coordinator.code = document.code
    context.coordinator.sourceID = document.sourceID
    context.coordinator.generation += 1
    view.loadHTMLString(document.html, baseURL: nil)
  }
  static func dismantleNSView(_ view: WKWebView, coordinator: Coordinator) {
    coordinator.generation += 1
    coordinator.webView = nil
    view.stopLoading()
    view.navigationDelegate = nil
  }
  final class Coordinator: NSObject, WKNavigationDelegate {
    var height: Binding<CGFloat>
    var html = ""
    var code: [String] = []
    var sourceID = ""
    var generation = 0
    weak var webView: WKWebView?
    init(height: Binding<CGFloat>) { self.height = height }
    func webView(_ webView: WKWebView, didFinish navigation: WKNavigation!) {
      measure()
      // WebKit's local font/layout pass can settle after didFinish without a
      // native width change. Keep those measurements bound to this document.
      let token = generation
      for delay in [0.1, 0.3] {
        DispatchQueue.main.asyncAfter(deadline: .now() + delay) { [weak self, weak webView] in
          guard let self, let webView, self.webView === webView, self.generation == token else { return }
          self.measure()
        }
      }
    }
    func measure() {
      guard let view = webView, !view.isLoading else { return }
      let token = generation
      // Host-only measurement in an isolated content world. The document has
      // no script, message handlers, file access or remote-resource permission.
      view.evaluateJavaScript("Math.ceil(document.querySelector('article').getBoundingClientRect().height)",
        in: nil, in: .defaultClient) { [weak self, weak view] result in
        guard let self, let view, self.webView === view, self.generation == token,
          case .success(let value) = result, let number = value as? NSNumber else { return }
        let next = max(24, CGFloat(number.doubleValue))
        guard next.isFinite, abs(self.height.wrappedValue - next) > 1 else { return }
        self.height.wrappedValue = next
      }
    }
    func webView(_ webView: WKWebView, decidePolicyFor action: WKNavigationAction,
      decisionHandler: @escaping (WKNavigationActionPolicy) -> Void) {
      guard let url = action.request.url else { decisionHandler(.cancel); return }
      if action.navigationType == .other && url.absoluteString == "about:blank" {
        decisionHandler(.allow); return
      }
      decisionHandler(.cancel)
      guard action.navigationType == .linkActivated else { return }
      let copyPrefix = "dunlin-copy:" + sourceID + "/"
      if url.absoluteString.hasPrefix(copyPrefix),
        let index = Int(url.absoluteString.dropFirst(copyPrefix.count)), code.indices.contains(index) {
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(code[index], forType: .string)
      } else if let link = ChatHTMLDocument.externalLink(url.absoluteString) {
        NSWorkspace.shared.open(link)
      }
    }
  }
}

private final class ChatRenderingWebView: WKWebView {
  var onResize: (() -> Void)?
  override func scrollWheel(with event: NSEvent) {
    if abs(event.scrollingDeltaY) >= abs(event.scrollingDeltaX) {
      nextResponder?.scrollWheel(with: event)
    } else { super.scrollWheel(with: event) }
  }
  override func setFrameSize(_ size: NSSize) {
    let changed = abs(size.width - frame.width) > 1
    super.setFrameSize(size)
    if changed { DispatchQueue.main.async { [weak self] in self?.onResize?() } }
  }
}

struct ChatHTMLContent: View {
  @Environment(\.chatHTMLPalette) private var palette
  @Environment(\.chatBodySize) private var bodySize
  @Environment(\.colorScheme) private var scheme
  let source: String
  @State private var height: CGFloat = 48
  var body: some View {
    VStack(alignment: .leading, spacing: 12) {
      ChatHTMLView(document: ChatHTMLDocument(source, dark: scheme == .dark,
        fontSize: Double(bodySize),palette:palette), height: $height)
        .frame(height: height).accessibilityIdentifier("chat.html-reply")
      ChatCopyButton(text: source, label: "复制回答")
    }.frame(maxWidth: .infinity, alignment: .leading)
  }
}

private struct ChatHTMLPaletteKey:EnvironmentKey {static let defaultValue:[String:String]=[:]}
extension EnvironmentValues {
  var chatHTMLPalette:[String:String] {
    get {self[ChatHTMLPaletteKey.self]}
    set {self[ChatHTMLPaletteKey.self]=newValue}
  }
}
