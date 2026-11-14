import AppKit
import SwiftUI
import UniformTypeIdentifiers
import CDesktop

struct FileHistoryStatus: Decodable {
  let operations: Int64
  let blobBytes: Int64
  let pendingStages: Int64
  let cleaned: Int?
}
struct PendingFileStage: Decodable, Identifiable {
  let id: String
  let root: String
  let path: String
  let created: Int64
}
struct WorkspaceMigration: Decodable {
  let version: Int
  let backup: String
  let backupBytes: Int64
  let backupExists: Bool
  let kind: String
  let created: Int64
}
struct DataSettingsView: View {
  @EnvironmentObject var store: WorkspaceStore
  @Environment(\.dismiss) private var dismiss
  @Environment(\.openWindow) private var openWindow
  @State private var maintenance = false
  @State private var deletion: DeletionReview?
  @State private var feedback = ""

  private struct DeletionReview: Identifiable {
    let id = UUID()
    let epoch: Int64
    let chats: [Chat]
    let identity: String
  }

  var body: some View {
    VStack(alignment: .leading, spacing: 26) {
      card("文件访问", height: 151, lines: [
        "普通聊天：只接收你主动添加的文本或 PNG。",
        "项目聊天：支持编辑文件与执行命令，授权范围可以独立调整。",
        "修改有差异记录，命令展示工作目录、参数、输出与退出码。",
      ]) { EmptyView() }
      card("本机存储", height: 138, lines: [
        "聊天、项目与索引保存在此 Mac，发送时内容交给所选模型服务。",
        "模型 API Key 保存在 macOS 钥匙串。导出日志前可预览内容。",
      ]) {
        action("在 Finder 中显示", id: "data.reveal") {
          NSWorkspace.shared.activateFileViewerSelecting([store.directory])
        }
      }
      card("数据管理", height: 149, lines: [
        "聊天可导出为 Markdown 或 JSON，导出范围为对话文本。",
        "清除搜索索引后可从已提交历史重建，原始聊天保留。",
      ]) {
        VStack(spacing: 10) {
          action("重建搜索索引", id: "data.rebuildIndex") { store.confirmIndexRebuild() }
          action("查看归档", id: "data.archive", action: showArchive)
        }
      }
      if !store.indexWarning.isEmpty { Text(store.indexWarning).foregroundStyle(.orange) }
      VStack(alignment: .leading, spacing: 12) {
        HStack {
          Button("删除全部本机聊天…", role: .destructive) {
            deletion = DeletionReview(epoch: store.identityEpoch, chats: store.chats,
              identity: store.currentIdentity?.name ?? "当前身份")
          }.buttonStyle(.plain).foregroundStyle(.red)
            .disabled(!store.chatCatalogLoaded || store.chats.isEmpty || store.deletingChats)
            .accessibilityIdentifier("data.deleteAll")
          Spacer()
          Button("数据维护…") { maintenance = true }.buttonStyle(.link)
            .accessibilityIdentifier("data.maintenance")
        }
        Text("需要二次确认。仅清理当前身份的聊天，项目目录中的文件保留。")
          .font(.callout).foregroundStyle(.secondary)
        if !feedback.isEmpty { Text(feedback).font(.callout).textSelection(.enabled) }
      }.padding(.top, 14)
    }.padding(.top, 8)
    .sheet(isPresented: $maintenance) {
      VStack(alignment: .leading) {
        HStack { Text("数据维护").font(.title2.bold()); Spacer(); Button("完成") { maintenance = false } }
        DataMaintenanceView()
      }.padding(24).frame(width: 660, height: 560)
    }
    .sheet(item: $deletion) { plan in
      VStack(alignment: .leading, spacing: 16) {
        Text("删除全部本机聊天？").font(.title2.bold())
        Text("将从「\(plan.identity)」清理已列出的 \(plan.chats.count) 个聊天，包括已归档的聊天。")
        Text("聊天原文、草稿及其派生记录将被清理。项目文件保留。进行中或有未决操作的聊天会保留并列入结果。")
        HStack {
          Button("取消") { deletion = nil }.keyboardShortcut(.cancelAction)
          Spacer()
          Button("确认删除 \(plan.chats.count) 个聊天", role: .destructive) {
            deletion = nil
            guard store.identityEpoch == plan.epoch else { return }
            store.deleteReviewedChats(plan.chats) { count in
              feedback = "已清理 \(count) 个聊天。"
              if count < plan.chats.count { feedback += "另有 \(plan.chats.count-count) 个未清理，请核对任务或存储状态后重试。" }
            }
          }.accessibilityIdentifier("data.confirmDelete")
        }
      }.padding(24).frame(width: 480)
    }
  }

  private func showArchive() {
    store.route = "archive"
    dismiss()
    if let window = NSApp.windows.first(where: { $0.identifier?.rawValue.contains("workspace") == true }) {
      window.makeKeyAndOrderFront(nil)
    } else { openWindow(id: "workspace") }
  }

  private func action(_ title: String, id: String, action: @escaping () -> Void) -> some View {
    Button(action: action) { Text(title).frame(width: 144) }
      .controlSize(.large).accessibilityIdentifier(id)
  }

  private func card<Actions: View>(_ title: String, height: CGFloat, lines: [String],
    @ViewBuilder actions: () -> Actions) -> some View {
    HStack(spacing: 18) {
      VStack(alignment: .leading, spacing: 7) {
        Text(title).font(.title3.weight(.semibold)).padding(.bottom, 4)
        ForEach(lines, id: \.self) { Text($0).font(.system(size: 14)).foregroundStyle(.secondary) }
      }.frame(maxWidth: .infinity, alignment: .leading)
      actions()
    }.padding(.horizontal, 22).padding(.vertical, 16).frame(minHeight: height, alignment: .topLeading)
      .background(store.themeSurface, in: RoundedRectangle(cornerRadius: 12))
      .overlay { RoundedRectangle(cornerRadius: 12).strokeBorder(.separator.opacity(0.55)) }
  }
}

private struct DataMaintenanceView: View {
  @EnvironmentObject var store: WorkspaceStore
  @State private var status: FileHistoryStatus?
  @State private var pending: [PendingFileStage] = []
  @State private var working = false
  @State private var error = ""
  @State private var migration: WorkspaceMigration?
  var body: some View {
    Form {
      Section("项目文件历史") {
        if let status {
          LabeledContent("文件操作记录", value: "\(status.operations) 条")
          LabeledContent(
            "去重后的快照文本",
            value: ByteCountFormatter.string(fromByteCount: status.blobBytes, countStyle: .file))
          Text("完整前后快照用于查看与回退；数据库页面和日志占用另计。删除聊天会清理它的历史，已执行的文件修改和命令效果保留。").font(.caption)
            .foregroundStyle(.secondary)
        }
      }
      Section("临时文件清理") {
        LabeledContent("待核对与清理", value: "\(status?.pendingStages ?? 0) 项")
        Button("重试临时文件清理") { cleanup() }.disabled(working || store.busy).accessibilityIdentifier(
          "data.cleanupStages")
        if working { ProgressView().controlSize(.small) }
        if (status?.pendingStages ?? 0) > 0 {
          Text("原目录不可用、文件不匹配或仍被使用时，会保留待办及验证所需快照。清理仅处理已验证属于应用的暂存文件。").font(.caption).foregroundStyle(
            .secondary)
          ForEach(pending) { item in
            VStack(alignment: .leading, spacing: 5) {
              Text(item.path).font(.callout)
              Text(item.root).font(.caption).foregroundStyle(.secondary).textSelection(.enabled)
            }
          }
          if (status?.pendingStages ?? 0) > 100 {
            Text("这里展示前 100 项，清理按批次继续。").font(.caption).foregroundStyle(.secondary)
          }
        }
      }
      Section("授权与本地数据") {
        Button("查看对话授权记录") { store.settingsSection = "授权记录" }
        Button("管理聊天检索索引") { store.settingsSection = "聊天检索" }
        Button("管理运行轨迹缓存") { store.settingsSection = "运行轨迹" }
        Button("打开系统设置…") {
          if let url = NSWorkspace.shared.urlForApplication(
            withBundleIdentifier: "com.apple.systempreferences")
          {
            NSWorkspace.shared.openApplication(at: url, configuration: .init())
          }
        }
        Text("系统设备权限在系统设置中管理。模型密钥在系统钥匙串保存，可从对应模型配置中管理。").font(.caption).foregroundStyle(.secondary)
      }
      Section("数据格式与回滚") {
        Text("旧客户端写入此数据会被阻止。回滚时请保留当前数据，并在另一个数据目录中使用升级前副本。")
          .font(.callout)
        if let migration {
          if migration.kind == "upgrade_backup" {
            LabeledContent("升级前数据副本", value: ByteCountFormatter.string(fromByteCount: migration.backupBytes, countStyle: .file))
            Text(migration.backup).font(.caption).foregroundStyle(.secondary).textSelection(.enabled)
            if migration.backupExists {
              Button("在访达中显示数据副本") {
                NSWorkspace.shared.activateFileViewerSelecting([store.directory.appendingPathComponent(migration.backup)])
              }.accessibilityIdentifier("data.upgradeBackup")
            } else {
              Text("副本已不在原位置，请核对已另存的数据。").font(.caption).foregroundStyle(.orange)
            }
          } else {
            Text(migration.kind == "initial_format" ? "此身份从当前格式创建，没有升级前副本。" : "未找到早期升级记录；当前数据保留。")
              .font(.caption).foregroundStyle(.secondary)
          }
        }
        Text("数据副本包含聊天和配置引用；恢复副本不会撤销文件修改或已执行命令。").font(.caption).foregroundStyle(.secondary)
      }
      if !error.isEmpty { Section { Text(error).foregroundStyle(.orange) } }
    }.formStyle(.grouped).onAppear { reload() }
  }
  private func reload() {
    store.readLocalView(["op": "chats.workspace.migration"], as: WorkspaceMigration.self) { r in
      if r.ok { migration = r.data } else { error = "数据升级记录暂不可读。" }
    }
    store.readLocalView(["op": "receipts.status"], as: FileHistoryStatus.self) { r in
      if r.ok { status = r.data } else { error = fileHistoryError(r.error) }
    }
    store.readLocalView(["op": "receipts.pending"], as: [PendingFileStage].self) { r in
      if r.ok { pending = r.data ?? [] }
    }
  }
  private func cleanup() {
    working = true
    store.workspaceRequest(["op": "receipts.cleanup"], as: FileHistoryStatus.self) { r in
      working = false
      if r.ok {
        status = r.data
        error = ""
        reload()
      } else {
        error = fileHistoryError(r.error)
      }
    }
  }
}

enum TranscriptFormat: CaseIterable {
  case markdown, json
  var label: String { self == .markdown ? "Markdown…" : "JSON…" }
  var suffix: String { self == .markdown ? "md" : "json" }

  func contents(title: String, messages: [Message]) throws -> Data {
    struct Turn: Encodable { let role: String; let content: String }
    struct Transcript: Encodable { let title: String; let messages: [Turn] }
    let turns = messages.filter { $0.role == "user" || $0.role == "assistant" }.map {
      Turn(role: $0.role, content: $0.content)
    }
    switch self {
    case .json:
      let encoder = JSONEncoder()
      encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
      return try encoder.encode(Transcript(title: title, messages: turns))
    case .markdown:
      let sections = turns.map { "## " + ($0.role == "user" ? "用户" : "助手") + "\n\n" + $0.content }
      return Data((["# " + title] + sections).joined(separator: "\n\n").appending("\n").utf8)
    }
  }
}

extension WorkspaceStore {
  func exportTranscript(_ chat: Chat, format: TranscriptFormat) {
    guard !busy, !workspaceTransitioning, let handle else { return }
    let epoch = identityEpoch
    let panel = NSSavePanel()
    panel.title = "导出聊天"
    panel.message = "保存聊天中的用户和助手文本。请在分享前检查正文。"
    panel.allowedContentTypes = [format == .json ? .json : UTType(filenameExtension: "md") ?? .plainText]
    panel.nameFieldStringValue = chat.title.replacingOccurrences(of: "/", with: "-") + "." + format.suffix
    guard panel.runModal() == .OK, let url = panel.url, identityEpoch == epoch else { return }
    queue.async {
      let rows = self.decode(dc_messages(handle.raw, chat.id), as: [Message].self)
      do {
        guard let rows else { throw CocoaError(.fileReadCorruptFile) }
        try format.contents(title: chat.title, messages: rows).write(to: url, options: .atomic)
      } catch {
        DispatchQueue.main.async { if self.identityEpoch == epoch { self.error = "聊天导出失败：" + error.localizedDescription } }
      }
    }
  }
}
