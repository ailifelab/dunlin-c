import AppKit
import SwiftUI

struct ChatWorkspaceReview: Decodable, Identifiable {
  var id: String { chat + ":" + String(bindingRevision) + ":" + String(projectRevision) + ":" + historyHash + ":" + path + ":" + device + ":" + inode }
  let chat: String
  let path: String
  let device: String
  let inode: String
  let currentPath: String
  let currentDevice: String
  let currentInode: String
  let project: String
  let bindingRevision: Int64
  let projectRevision: Int64
  let historyHash: String
  let messageCount: Int
  let canApply: Bool
  let reason: String
}

extension WorkspaceStore {
  func chooseChatWorkspace() {
    guard !busy, !creatingChat, !switchingModel, !workspacePlanning, !workspaceSaving,
      let chat = selectedChat, !chat.active else { return }
    let panel = NSOpenPanel()
    panel.canChooseFiles = false
    panel.canChooseDirectories = true
    panel.canCreateDirectories = true
    panel.prompt = "选择此聊天的工作区"
    panel.message = "只更改这段聊天的文件夹；项目默认目录和已保存的聊天不会改变。"
    guard panel.runModal() == .OK, let url = panel.url else { return }
    previewChatWorkspace(chat: chat.id, path: url.resolvingSymlinksInPath().path)
  }
  func previewChatWorkspace(chat: String, path: String) {
    guard !busy, !creatingChat, !switchingModel, !workspacePlanning, !workspaceSaving,
      selected == chat else { return }
    workspacePlanning = true
    workspaceReviewError = ""
    workspaceRequest(["op": "chats.workspace.preview", "chat": chat, "path": path], as: ChatWorkspaceReview.self) { result in
      self.workspacePlanning = false
      guard self.selected == chat else { return }
      if result.ok, let review = result.data {
        self.workspaceReview = review
      } else {
        let message = self.workspaceBindingError(result.error)
        self.workspaceReviewError = message
        if self.workspaceReview == nil { self.error = message }
      }
    }
  }
  func cancelChatWorkspaceReview() {
    guard !workspaceSaving, !workspacePlanning else { return }
    workspaceReview = nil
    workspaceReviewError = ""
  }
  func applyChatWorkspace(_ review: ChatWorkspaceReview) {
    guard !busy, !workspacePlanning, !workspaceSaving, selected == review.chat,
      workspaceReview?.id == review.id, review.canApply else { return }
    workspaceSaving = true
    workspaceReviewError = ""
    let body: [String: Any] = [
      "op": "chats.workspace.apply", "chat": review.chat, "path": review.path,
      "device": review.device, "inode": review.inode, "project": review.project,
      "bindingRevision": review.bindingRevision, "projectRevision": review.projectRevision,
      "historyHash": review.historyHash,
    ]
    workspaceRequest(body, as: ChatWorkspaceReview.self) { result in
      self.workspaceSaving = false
      guard self.selected == review.chat else { return }
      if result.ok {
        self.workspaceReview = nil
        self.workspaceReviewError = ""
        self.invalidateProjectContent()
        self.reload()
        self.reloadProjects()
      } else {
        self.workspaceReviewError = self.workspaceBindingError(result.error)
      }
    }
  }
  private func workspaceBindingError(_ code: String?) -> String {
    switch code {
    case "active_checkpoint": return "此聊天仍有未完成的回合，请先处理恢复状态。"
    case "workspace_review_stale": return "聊天、目录或项目属性已变化，请重新检查后确认。"
    case "invalid_workspace_path": return "目录不可访问，或与应用私有数据目录重叠，请选择独立的文件夹。"
    case "invalid_workspace_checkpoint_or_binding": return "聊天工作区或原始记录不可核对，数据保留，请先检查恢复状态。"
    default: return "未能更改聊天工作区：" + (code ?? "本地存储错误")
    }
  }
}

struct ChatWorkspaceView: View {
  @EnvironmentObject var store: WorkspaceStore
  let review: ChatWorkspaceReview
  var body: some View {
    VStack(alignment: .leading, spacing: 24) {
      Text("重新关联此聊天的工作区").font(.system(size: 23, weight: .semibold))
      Text("项目默认目录保持不变。此操作只影响这段聊天后续的文件与命令操作。")
        .font(.callout).foregroundStyle(.secondary)
      VStack(alignment: .leading, spacing: 18) {
        folder("当前聊天工作区", path: review.currentPath)
        Divider()
        folder("新工作区", path: review.path)
      }.padding(20).background(Color(nsColor: .textBackgroundColor), in: RoundedRectangle(cornerRadius: 12))
      VStack(alignment: .leading, spacing: 12) {
        Label(review.canApply ? "已核对此次目录选择" : "请先处理未完成的回合",
              systemImage: review.canApply ? "checkmark.circle" : "exclamationmark.triangle")
          .font(.headline)
        Text("\(review.messageCount) 条原始记录、已执行操作和当前草稿会保留。")
        Text("不会复制文件或重跑历史工具。后续修改文件、执行命令仍按操作授权。")
      }.font(.callout).foregroundStyle(review.canApply ? Color.accentColor : .orange)
        .padding(20).frame(maxWidth: .infinity, alignment: .leading)
        .background(Color.accentColor.opacity(0.06), in: RoundedRectangle(cornerRadius: 12))
      if !store.workspaceReviewError.isEmpty {
        Text(store.workspaceReviewError).font(.callout).foregroundStyle(.red).textSelection(.enabled)
          .accessibilityIdentifier("workspace.review.error")
      }
      HStack {
        Button("重新检查") { store.previewChatWorkspace(chat: review.chat, path: review.path) }
        Spacer()
        Button("取消", action: store.cancelChatWorkspaceReview).keyboardShortcut(.cancelAction)
        Button(store.workspaceSaving ? "正在关联…" : "仅重新关联此聊天") { store.applyChatWorkspace(review) }
          .buttonStyle(.borderedProminent).disabled(!review.canApply).keyboardShortcut(.defaultAction)
          .accessibilityIdentifier("workspace.review.apply")
      }.disabled(store.workspaceSaving || store.workspacePlanning)
    }.padding(32).frame(width: 740).interactiveDismissDisabled(store.workspaceSaving || store.workspacePlanning)
      .accessibilityIdentifier("workspace.review")
  }
  private func folder(_ title: String, path: String) -> some View {
    VStack(alignment: .leading, spacing: 8) {
      Text(title).font(.caption).foregroundStyle(.secondary)
      Label(path, systemImage: "folder").font(.callout).textSelection(.enabled)
    }
  }
}
