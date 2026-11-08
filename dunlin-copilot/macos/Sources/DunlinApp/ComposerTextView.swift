import AppKit
import SwiftUI

// NSTextInputClient remains responsible for marked text and candidate selection.
// Return is intercepted before interpretation only when no IME composition exists.
private final class ChatInputTextView: NSTextView {
  var sendKey: ChatSendKey = .returnKey
  var canSubmit = false
  var submit: () -> Void = {}
  var pastePNG: (Data) -> Void = { _ in }
  var unsupportedImage: () -> Void = {}
  override func paste(_ sender: Any?) {
    let board = NSPasteboard.general
    if let bytes = board.data(forType: .png) { pastePNG(bytes); return }
    if board.availableType(from: [.tiff]) != nil, board.string(forType: .string) == nil {
      unsupportedImage(); return
    }
    super.paste(sender)
  }
  override func keyDown(with event: NSEvent) {
    if event.keyCode == 36 || event.keyCode == 76 {
      let flags = event.modifierFlags
      var modifiers: ComposerModifiers = []
      if flags.contains(.shift) { modifiers.insert(.shift) }
      if flags.contains(.command) { modifiers.insert(.command) }
      if flags.contains(.option) { modifiers.insert(.option) }
      if flags.contains(.control) { modifiers.insert(.control) }
      if ComposerInputPolicy.sendsReturn(
        policy: sendKey, modifiers: modifiers,
        markedText: hasMarkedText(), canSubmit: canSubmit)
      {
        submit()
        return
      }
    }
    super.keyDown(with: event)
  }
}
private final class ChatInputScrollView: NSScrollView {
  override func tile() {
    super.tile()
    guard let editor = documentView as? NSTextView else { return }
    editor.minSize = NSSize(width: 0, height: contentSize.height)
    let size = NSSize(
      width: contentSize.width, height: max(editor.frame.height, contentSize.height))
    if editor.frame.size != size { editor.setFrameSize(size) }
  }
}
struct ComposerTextView: NSViewRepresentable {
  @Binding var text: String
  let sendKey: ChatSendKey
  let textSize: Double
  let enabled: Bool
  let canSubmit: Bool
  let submit: () -> Void
  var pastePNG: (Data) -> Void = { _ in }
  var unsupportedImage: () -> Void = {}
  final class Coordinator: NSObject, NSTextViewDelegate {
    var parent: ComposerTextView
    init(_ parent: ComposerTextView) { self.parent = parent }
    func textDidChange(_ notification: Notification) {
      guard let editor = notification.object as? NSTextView else { return }
      parent.text = editor.string
    }
  }
  func makeCoordinator() -> Coordinator { Coordinator(self) }
  func makeNSView(context: Context) -> NSScrollView {
    let scroll = ChatInputScrollView()
    let editor = ChatInputTextView(frame: .zero)
    scroll.drawsBackground = false
    scroll.hasVerticalScroller = true
    scroll.autohidesScrollers = true
    scroll.borderType = .noBorder
    editor.isRichText = false
    editor.importsGraphics = false
    editor.drawsBackground = false
    editor.isVerticallyResizable = true
    editor.isHorizontallyResizable = false
    editor.autoresizingMask = [.width]
    editor.minSize = .zero
    editor.maxSize = NSSize(
      width: CGFloat.greatestFiniteMagnitude, height: CGFloat.greatestFiniteMagnitude)
    editor.textContainer?.widthTracksTextView = true
    editor.textContainer?.containerSize = NSSize(width: 0, height: CGFloat.greatestFiniteMagnitude)
    editor.textContainerInset = NSSize(width: 2, height: 4)
    editor.isAutomaticQuoteSubstitutionEnabled = false
    editor.isAutomaticDashSubstitutionEnabled = false
    editor.isAutomaticTextReplacementEnabled = false
    editor.isAutomaticSpellingCorrectionEnabled = false
    editor.allowsUndo = true
    editor.delegate = context.coordinator
    editor.setAccessibilityIdentifier("chat.input")
    editor.setAccessibilityLabel("输入消息")
    scroll.documentView = editor
    update(editor, context: context)
    return scroll
  }
  func updateNSView(_ scroll: NSScrollView, context: Context) {
    guard let editor = scroll.documentView as? ChatInputTextView else { return }
    update(editor, context: context)
  }
  private func update(_ editor: ChatInputTextView, context: Context) {
    context.coordinator.parent = self
    editor.sendKey = sendKey
    editor.canSubmit = enabled && canSubmit
    editor.submit = submit
    editor.pastePNG = pastePNG
    editor.unsupportedImage = unsupportedImage
    editor.isEditable = enabled
    editor.isSelectable = true
    editor.font = .systemFont(ofSize: textSize)
    editor.textColor = .labelColor
    if editor.string != text && !editor.hasMarkedText() {
      let selection = editor.selectedRange()
      editor.string = text
      let length = (text as NSString).length
      let location = min(selection.location, length)
      editor.setSelectedRange(
        NSRange(location: location, length: min(selection.length, length - location)))
      editor.undoManager?.removeAllActions()
    }
  }
}
