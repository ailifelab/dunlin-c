import AppKit
import SwiftUI

struct PublicReasoningView: View {
  @Environment(\.chatBodySize) private var bodySize
  let text: String
  let kind: String
  var unavailable = false
  @State private var expanded = false
  var body: some View {
    DisclosureGroup(isExpanded: $expanded) {
      if unavailable {
        Text("公开推理暂不可显示，原始记录已保留。").font(.callout).foregroundStyle(.secondary)
      } else {
        Text(text).font(.system(size: max(12, bodySize - 5))).lineSpacing(4).textSelection(.enabled)
          .frame(maxWidth: .infinity, alignment: .leading)
          .padding(.top, 10)
      }
    } label: {
      Text(kind == "summary" ? "公开推理摘要" : "公开推理")
        .font(.system(size: 13)).foregroundStyle(.secondary)
    }
    .tint(Color.secondary)
    .accessibilityIdentifier("chat.public-reasoning")
  }
}
struct ChatAssistantHeader: View {
  var showAvatar = true
  var modelName: String? = nil
  var body: some View {
    HStack(spacing: 13) {
      if showAvatar {
        Text("D").font(.system(size: 17, weight: .semibold)).foregroundStyle(
          Color(nsColor: .textBackgroundColor)
        )
        .frame(width: 29, height: 29).background(
          Color.primary, in: RoundedRectangle(cornerRadius: 8)
        )
        .accessibilityHidden(true)
      }
      Text("Dunlin Copilot").font(.system(size: showAvatar ? 15 : 14, weight: .semibold))
      if let modelName, !modelName.isEmpty {
        Spacer()
        Text(modelName).font(.system(size: 12)).foregroundStyle(.secondary).lineLimit(1)
          .truncationMode(.middle)
      }
    }
  }
}
struct ChatCopyButton: View {
  let text: String
  let label: String
  @State private var copied = false
  var body: some View {
    Button {
      let board = NSPasteboard.general
      board.clearContents()
      copied = board.setString(text, forType: .string)
    } label: {
      Label(copied ? "已复制" : label, systemImage: copied ? "checkmark" : "doc.on.doc")
    }
    .buttonStyle(.plain).font(.system(size: 12)).foregroundStyle(.secondary)
    .disabled(text.isEmpty).help(label).accessibilityLabel(copied ? "已复制" : label)
    .task(id: copied) {
      guard copied else { return }
      try? await Task.sleep(for: .seconds(1.6))
      if !Task.isCancelled { copied = false }
    }
  }
}
