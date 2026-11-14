import AppKit
import SwiftUI

struct PublicReasoningView: View {
  @Environment(\.chatBodySize) private var bodySize
  @EnvironmentObject private var store: WorkspaceStore
  let text: String
  let kind: String
  var unavailable = false
  var sourceChat: String? = nil
  var sequence: Int? = nil
  @State private var original = false
  @State private var expanded = false
  var body: some View {
    DisclosureGroup(isExpanded: $expanded) {
      if expanded { expandedBody }
    } label: {
      HStack {
        Text(kind == "summary" ? "公开推理摘要" : "公开推理")
          .font(.body).foregroundStyle(.secondary)
        if expanded, !unavailable, sourceChat != nil, sequence != nil, reasoningClient != nil {
          Spacer()
          Menu {
            Button(original ? "返回扩展视图" : "直接查看原文") { original.toggle() }
              .accessibilityIdentifier("reasoning.original")
          } label: {
            Image(systemName: "ellipsis.circle")
          }
          .fixedSize().menuStyle(.borderlessButton).menuIndicator(.hidden)
          .accessibilityLabel("推理显示选项")
        }
      }
    }
    .tint(Color.secondary)
    .accessibilityIdentifier("chat.public-reasoning")
  }

  private var reasoningClient: PluginClient? {
    store.pluginClients.first { $0.slots?.contains("conversation.chat.reasoning.body") == true }
  }

  @ViewBuilder private var expandedBody: some View {
    if unavailable {
      Text("公开推理暂不可显示，原始记录已保留。").font(.callout).foregroundStyle(.secondary)
    } else if !original, let chat = sourceChat, let sequence,
      let client = reasoningClient {
      PluginReasoningBody(client: client, chat: chat, sequence: sequence, text: text)
        .id("\(store.identityEpoch):\(chat):\(sequence):\(client.ownerID)")
    } else {
      Text(text).font(.system(size: max(12, bodySize - 1))).lineSpacing(2).textSelection(.enabled)
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(.top, 10)
    }
  }
}
struct PluginReasoningBody: View {
  @EnvironmentObject private var store: WorkspaceStore
  @Environment(\.colorScheme) private var scheme
  let client: PluginClient
  let chat: String
  let sequence: Int
  let text: String
  @State private var height: CGFloat = 48
  var body: some View {
    PluginClientSurface(client: client, chat: chat, onFault: { diagnostic in
      store.extensionRequest(client.owner.merging(["op":"extensions.clientFault","diagnostic":diagnostic]) { _, new in new },
                             as: [ExtensionRow].self) { _ in store.refreshPluginPages(); store.pollExtensionHealth() }
    }, slot: "conversation.chat.reasoning.body", reasoningText: text,
      theme: scheme == .dark ? "dark" : "light", themeDefinition: store.pluginThemeDefinition,
      onContentHeight: { height = $0 },
      onRequest: { command, args, reply in
        guard store.selected == chat, store.showReasoning,
              store.messages.contains(where: { $0.sequence == sequence && $0.reasoning == text }),
              store.pluginClients.contains(where: { $0.ownerID == client.ownerID }) else {
          reply(nil,"当前会话或插件已变化"); return
        }
        if command == "reasoning.service" {
          guard let method = args["method"] as? String, ["limits","translate"].contains(method) else {
            reply(nil,"未知翻译服务方法"); return
          }
          var parameters: [[String:Any]] = []
          if method == "translate" {
            guard let request = args["request"] as? [String:Any], let source = request["text"] as? String,
              !source.isEmpty, text.contains(source), request["sessionId"] as? String == chat else {
              reply(nil,"翻译来源已变化"); return
            }
            parameters = [request]
          }
          store.pluginClientRequest(client, chat: chat, command: "rpc",
            args: ["namespace":"cotTranslation","method":method,"args":parameters], reply: reply)
        } else if ["storage.read","storage.write","theme.presentation","config.set"].contains(command) {
          store.pluginClientRequest(client, chat: chat, command: command, args: args, reply: reply)
        } else {
          reply(nil,"公开推理扩展未声明此服务")
        }
      })
      .frame(height: height)
      .accessibilityIdentifier("reasoning.extension")
  }
}
struct ChatAssistantHeader: View {
  var showAvatar = true
  var modelName: String? = nil
  var body: some View {
    HStack(spacing: 8) {
      if showAvatar {
        Text("D").font(.title3.weight(.semibold)).foregroundStyle(
          Color(nsColor: .textBackgroundColor)
        )
        .frame(width: 29, height: 29).background(
          Color.primary, in: RoundedRectangle(cornerRadius: 8)
        )
        .accessibilityHidden(true)
      }
      Text("Dunlin Copilot").font(.headline)
      if let modelName, !modelName.isEmpty {
        Spacer()
        Text(modelName).font(.callout).foregroundStyle(.secondary).lineLimit(1)
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
    .buttonStyle(.plain).font(.callout).foregroundStyle(.secondary)
    .disabled(text.isEmpty).help(label).accessibilityLabel(copied ? "已复制" : label)
    .task(id: copied) {
      guard copied else { return }
      try? await Task.sleep(for: .seconds(1.6))
      if !Task.isCancelled { copied = false }
    }
  }
}
