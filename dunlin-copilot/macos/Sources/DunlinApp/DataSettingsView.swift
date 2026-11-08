import AppKit
import SwiftUI

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
