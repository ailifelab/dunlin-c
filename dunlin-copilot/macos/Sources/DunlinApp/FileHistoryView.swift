import AppKit
import SwiftUI

struct FileReceipt: Decodable, Identifiable {
  let cursor: Int64
  let id: String
  let run: String
  let call: String
  let proposal: Int64
  let grant: Int64
  let project: String
  let root: String
  let device: String
  let inode: String
  let path: String
  let operation: String
  let beforeHash: String
  let afterHash: String
  let existed: Bool
  let afterExists: Bool
  let parent: String
  let state: String
  let detail: String
  let revision: Int64
  let created: Int64
  let finished: Int64
  let before: String?
  let after: String?
  let currentState: String?
  let currentHash: String?
  let currentRoot: String?
  let canRestore: Bool?
  let unknownAcknowledgementRequired: Bool?
  let plan: String?
  let planLifetimeSeconds: Int?
  var status: String {
    switch state {
    case "applied": return afterExists ? "变更已提交" : "移除已提交"
    case "prepared": return "结果未结算"
    case "unknown": return "效果未决"
    case "failed": return "未发布成功"
    default: return state
    }
  }
  var color: Color { state == "applied" ? .green : state == "failed" ? .secondary : .orange }
}
struct FileReceiptPage: Decodable {
  let items: [FileReceipt]
  let more: Bool
  let nextCursor: Int64
}
struct FileReceiptSelection: Identifiable {
  let id: String
  let chat: String
}
struct FileRestoreResult: Decodable {
  let status: String
  let result: String
  let operationId: String?
}
func fileHistoryError(_ value: String?) -> String {
  switch value {
  case "file_restore_conflict_or_active": return "文件已发生后续变化，或原回合尚未结束。请刷新并核对当前文件。"
  case "file_restore_conflict_or_failed": return "回退未完成；文件哈希、目录或写入条件可能已经变化。原始快照仍保留。"
  case "file_restore_outcome_unknown": return "回退效果尚未结算，文件可能已改变。请核对当前文件，不要重复提交旧计划。"
  case "file_restore_plan_stale": return "回退计划已过期、已使用或来源已变化，请重新预览。"
  case "file_history_snapshot_invalid": return "文件历史快照未通过校验，已停止回退。"
  case "file_unknown_acknowledgement_required": return "请先核对实际文件，并确认对未决效果的理解。"
  case "active_checkpoint": return "原回合尚未结束，请先处理恢复与未决结果。"
  case "file_history_storage_error": return "文件历史读写未完成，原始数据保持保留。"
  case "file_receipt_not_found", "chat_not_found": return "这条文件历史或所属聊天已移除。"
  default: return value ?? "文件历史操作未完成"
  }
}
extension WorkspaceStore {
  func reloadReceipts(older: Bool = false) {
    guard let chat = selected else {
      receiptRows = []
      receiptMore = false
      return
    }
    let token = UUID()
    receiptRequestID = token
    var body: [String: Any] = ["op": "receipts.list", "chat": chat]
    if older { body["before"] = receiptCursor }
    readLocalView(body, as: FileReceiptPage.self) { r in
      guard self.selected == chat, self.receiptRequestID == token else { return }
      if let page = r.data, r.ok {
        self.receiptRows = older ? self.receiptRows + page.items : page.items
        self.receiptMore = page.more
        self.receiptCursor = page.nextCursor
        self.receiptError = ""
      } else {
        self.receiptError = fileHistoryError(r.error)
      }
    }
  }
}
struct FileHistoryListView: View {
  @EnvironmentObject var store: WorkspaceStore
  @Environment(\.dismiss) private var dismiss
  @State private var chat = ""
  @State private var selection: FileReceiptSelection?
  var body: some View {
    VStack(alignment: .leading, spacing: 18) {
      HStack {
        Text("文件变更记录").font(.title2.weight(.semibold))
        Spacer()
        Button("刷新") { store.reloadReceipts() }
        Button("完成") { dismiss() }
      }
      Text("当前聊天的受控文件工具与手动回退记录。命令产生的其他文件变化需另行核对，不能从这里推断全部工作区状态。").font(.callout).foregroundStyle(
        .secondary)
      ScrollView {
        LazyVStack(alignment: .leading, spacing: 10) {
          ForEach(store.receiptRows) { r in
            Button {
              selection = FileReceiptSelection(id: r.id, chat: chat)
            } label: {
              HStack(spacing: 14) {
                Image(
                  systemName: r.operation == "restore_file"
                    ? "arrow.uturn.backward.circle" : "doc.text"
                ).font(.title2).foregroundStyle(r.color)
                VStack(alignment: .leading, spacing: 6) {
                  Text(r.path).fontWeight(.medium)
                  Text(
                    Date(timeIntervalSince1970: Double(r.created)),
                    format: .dateTime.month().day().hour().minute().second()
                  ).font(.caption).foregroundStyle(.secondary)
                }
                Spacer()
                Text(r.status).font(.callout).foregroundStyle(r.color)
                Image(systemName: "chevron.right").font(.caption).foregroundStyle(.secondary)
              }.padding(16).background(
                Color.primary.opacity(0.035), in: RoundedRectangle(cornerRadius: 12))
            }.buttonStyle(.plain).accessibilityIdentifier("receipt.row.\(r.id)")
          }
          if store.receiptMore { Button("加载更早的记录") { store.reloadReceipts(older: true) } }
          if store.receiptRows.isEmpty {
            ContentUnavailableView(
              "还没有 C 文件回执", systemImage: "doc.text.magnifyingglass",
              description: Text("后续文件写入、补丁和回退会保留可校验的前后快照。"))
          }
        }
      }
      if !store.receiptError.isEmpty { Text(store.receiptError).foregroundStyle(.orange) }
    }.padding(26).frame(width: 820, height: 650).onAppear {
      chat = store.selected ?? ""
      store.reloadReceipts()
    }
    .onChange(of: store.identityEpoch) { _, _ in dismiss() }.onChange(of: store.selected) {
      _, value in if value != chat { dismiss() }
    }
    .sheet(item: $selection) { FileReceiptView(selection: $0) }
  }
}
struct FileReceiptView: View {
  let selection: FileReceiptSelection
  @EnvironmentObject var store: WorkspaceStore
  @Environment(\.dismiss) private var dismiss
  @State private var receipt: FileReceipt?
  @State private var candidate: FileReceipt?
  @State private var acknowledge = false
  @State private var working = false
  @State private var error = ""
  @State private var notice = ""
  @State private var requestID = UUID()
  @State private var activeReceiptID: String?
  private var receiptID: String { activeReceiptID ?? selection.id }
  private var chatActive: Bool { store.chats.first { $0.id == selection.chat }?.active ?? true }
  var body: some View {
    VStack(alignment: .leading, spacing: 18) {
      HStack {
        Label(receipt?.path ?? "文件变更", systemImage: "doc.text").font(.title3.weight(.semibold))
        Spacer()
        if let receipt { Text(receipt.status).foregroundStyle(receipt.color) }
        Button("完成") { dismiss() }.disabled(working)
      }
      if let receipt {
        Text("记录时项目：" + receipt.root).font(.caption).foregroundStyle(.secondary).textSelection(
          .enabled)
        if let candidate {
          Label("回退预览", systemImage: "arrow.uturn.backward.circle").font(.headline)
          Text("仅回退这个文件的内容。计划约 \(candidate.planLifetimeSeconds ?? 300) 秒内有效，确认时仍会重新检查文件哈希和目录身份。")
            .font(.callout).foregroundStyle(.secondary)
          if let currentRoot = candidate.currentRoot, !currentRoot.isEmpty {
            Text(
              "目标：" + URL(fileURLWithPath: currentRoot).appendingPathComponent(candidate.path).path
            ).font(.caption).textSelection(.enabled)
          }
          diff(
            before: candidate.afterExists ? candidate.after ?? "" : "（当前文件不存在）",
            after: candidate.existed ? candidate.before ?? "" : "（将移除这个新建文件；快照仍保留）",
            beforeTitle: "当前应匹配的内容", afterTitle: "回退后的内容")
          HStack {
            Button("返回差异") { self.candidate = nil }
            Spacer()
            if working { ProgressView().controlSize(.small) }
            Button("确认回退") { restore(candidate) }.buttonStyle(.borderedProminent).disabled(
              working || store.busy || chatActive
            ).accessibilityIdentifier("receipt.restore.confirm")
          }
        } else {
          diff(
            before: receipt.existed ? receipt.before ?? "" : "（当时文件不存在）",
            after: receipt.afterExists ? receipt.after ?? "" : "（该次操作移除了文件）", beforeTitle: "修改前",
            afterTitle: "修改后")
          if receipt.currentState != "matches_after" {
            Label(currentState(receipt.currentState), systemImage: "exclamationmark.triangle").font(
              .callout
            ).foregroundStyle(.orange)
          }
          if receipt.unknownAcknowledgementRequired == true {
            Toggle("我已核对实际文件，理解未决效果没有被判定成功、失败或撤销", isOn: $acknowledge).font(.callout)
          }
          if chatActive {
            Text("原回合仍未结束，请先处理恢复与未决结果，再考虑回退。").font(.caption).foregroundStyle(.orange)
          }
          HStack {
            Menu("保存完整快照…") {
              Button("修改前…") { save(receipt.before ?? "", suffix: "before") }.disabled(
                !receipt.existed)
              Button("修改后…") { save(receipt.after ?? "", suffix: "after") }.disabled(
                !receipt.afterExists)
            }
            Button("刷新状态") { reload() }.disabled(working)
            Spacer()
            Button(receipt.operation == "restore_file" ? "恢复回退前内容…" : "准备回退…") { prepare() }
              .buttonStyle(.borderedProminent).disabled(
                working || store.busy || chatActive || receipt.currentState != "matches_after"
                  || (!(receipt.canRestore ?? false) && !acknowledge)
              ).accessibilityIdentifier("receipt.restore.preview")
          }
          Text("回合 \(receipt.run.prefix(8)) · \(receipt.operation) · 修订 \(receipt.revision)").font(
            .caption.monospaced()
          ).foregroundStyle(.secondary)
        }
      } else if error.isEmpty {
        ProgressView().frame(maxWidth: .infinity, maxHeight: .infinity)
      } else {
        ContentUnavailableView(
          "文件历史暂不可用", systemImage: "doc.badge.ellipsis", description: Text("原始数据保持保留，请核对来源后重试。"))
      }
      if !notice.isEmpty { Text(notice).foregroundStyle(.green).font(.callout) }
      if !error.isEmpty {
        Text(error).foregroundStyle(.orange).font(.callout).textSelection(.enabled)
      }
    }.padding(26).frame(width: 1050, height: 730).onAppear { reload() }
      .onChange(of: store.identityEpoch) { _, _ in dismiss() }.onChange(of: store.selected) {
        _, value in if value != selection.chat { dismiss() }
      }
  }
  private func currentState(_ value: String?) -> String {
    switch value {
    case "conflict": return "当前文件已发生后续变化，不能直接回退这一版本。"
    case "project_unavailable": return "原目录未关联或目录身份已变化，请先在项目属性中核对。"
    default: return "当前文件无法安全读取，原始快照保留。"
    }
  }
  private func diff(before: String, after: String, beforeTitle: String, afterTitle: String)
    -> some View
  {
    HStack(alignment: .top, spacing: 1) {
      column(beforeTitle, before, .red)
      column(afterTitle, after, .green)
    }.frame(maxHeight: .infinity)
  }
  private func column(_ title: String, _ text: String, _ color: Color) -> some View {
    VStack(alignment: .leading, spacing: 12) {
      Text(title).font(.headline)
      if text.utf8.count > 65536 {
        Text("界面显示前 20,000 个字符；可保存完整快照。").font(.caption).foregroundStyle(.secondary)
      }
      ScrollView([.horizontal, .vertical]) {
        Text(text.utf8.count > 65536 ? String(text.prefix(20_000)) : text).font(
          .system(size: 12, design: .monospaced)
        ).textSelection(.enabled).frame(maxWidth: .infinity, alignment: .topLeading).padding(12)
      }
    }.padding(14).frame(maxWidth: .infinity, maxHeight: .infinity).background(
      color.opacity(0.035), in: RoundedRectangle(cornerRadius: 10))
  }
  private func reload(preserveError: Bool = false) {
    let token = UUID()
    requestID = token
    store.readLocalView(
      ["op": "receipts.detail", "chat": selection.chat, "id": receiptID], as: FileReceipt.self
    ) { r in
      guard requestID == token else { return }
      if let value = r.data, r.ok {
        receipt = value
        if !preserveError { error = "" }
      } else {
        let failure = fileHistoryError(r.error)
        if preserveError && !error.isEmpty && error != failure {
          error += "\n" + failure
        } else {
          error = failure
        }
      }
    }
  }
  private func prepare() {
    guard !working, !store.busy, !chatActive else { return }
    working = true
    notice = ""
    store.workspaceRequest(
      [
        "op": "receipts.preview", "chat": selection.chat, "id": receiptID,
        "acknowledgeUnknown": acknowledge,
      ], as: FileReceipt.self
    ) { r in
      working = false
      if r.ok {
        candidate = r.data
        error = ""
      } else {
        error = fileHistoryError(r.error)
        reload(preserveError: true)
      }
    }
  }
  private func restore(_ candidate: FileReceipt) {
    guard !working, !store.busy, !chatActive else { return }
    guard let plan = candidate.plan else { return }
    working = true
    error = ""
    store.workspaceRequest(
      ["op": "receipts.restore", "chat": selection.chat, "plan": plan, "confirmed": true],
      as: FileRestoreResult.self
    ) { r in
      working = false
      self.candidate = nil
      if let next = r.data?.operationId {
        activeReceiptID = next
        receipt = nil
        acknowledge = false
      }
      if r.ok { notice = "回退已提交，已追加新的文件记录。" } else { error = fileHistoryError(r.error) }
      store.reloadReceipts()
      store.reload()
      reload(preserveError: !r.ok)
    }
  }
  private func save(_ text: String, suffix: String) {
    let panel = NSSavePanel()
    panel.nameFieldStringValue =
      URL(fileURLWithPath: receipt?.path ?? "snapshot.txt").lastPathComponent + "." + suffix
      + ".txt"
    guard panel.runModal() == .OK, let url = panel.url else { return }
    do { try Data(text.utf8).write(to: url, options: .atomic) } catch {
      self.error = error.localizedDescription
    }
  }
}
