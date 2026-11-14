import SwiftUI

struct RecoveryState: Decodable {
  let stateHash: String
  let active: Bool
  let run: String
  let profile: String
  let pending: String
  let arguments: String
  let detail: String
  let unknownCount: Int64
}

struct RecoveryView: View {
  @EnvironmentObject var store: WorkspaceStore
  @Environment(\.dismiss) private var dismiss
  @State private var state: RecoveryState?
  @State private var chatID = ""
  @State private var acknowledge = false
  @State private var showsDetails = false
  @State private var error = ""
  @State private var busy = false

  var body: some View {
    VStack(alignment: .leading, spacing: 18) {
      if let state {
        if state.active && !state.pending.isEmpty && !showsDetails {
          unknownResult
        } else {
          checkpointDetails(state)
        }
      } else if error.isEmpty {
        ProgressView("正在读取恢复记录…")
      } else {
        Text("无法读取恢复记录").font(.title2.weight(.semibold))
        Button("关闭") { dismiss() }
      }
      if !error.isEmpty {
        Text(message(error)).font(.callout).foregroundStyle(.red).textSelection(.enabled)
      }
    }
    .padding(28)
    .frame(width: 680, alignment: .topLeading)
    .onAppear {
      chatID = store.selected ?? ""
      reload()
    }
    .onChange(of: store.identityEpoch) { _, _ in dismiss() }
  }

  private var unknownResult: some View {
    VStack(alignment: .leading, spacing: 18) {
      Text("这次操作的结果尚未确认").font(.title2.weight(.semibold))
      Text("上次运行在提交结果之前中断。").foregroundStyle(.secondary)
      VStack(alignment: .leading, spacing: 9) {
        Label("为避免重复执行，暂时不能重试。", systemImage: "exclamationmark.triangle")
          .font(.headline)
        Text("最后一个已保存的步骤：请求已发出，结果未完整提交。")
        Text("已提交的聊天仍然可读。请先检查外部工具的实际效果。")
      }
      .font(.callout).padding(18).frame(maxWidth: .infinity, alignment: .leading)
      .background(Color.orange.opacity(0.10), in: RoundedRectangle(cornerRadius: 10))
      HStack {
        Button("查看已保存的聊天") {
          if store.openChat(chatID) { dismiss() }
        }
        Button("查看恢复详情") { showsDetails = true }
          .accessibilityIdentifier("recovery.details")
      }
      Text("新聊天不会自动重复上次操作，原检查点会保留。")
        .font(.callout).foregroundStyle(.secondary)
      HStack {
        Spacer()
        Button("保留并关闭") { dismiss() }
        Button("开启新聊天") {
          // A disconnected project still allows reading its original chat.
          // Start an independent ordinary chat if its default folder is offline.
          let projectID = store.chats.first { $0.id == chatID }?.project
          let project = store.projects.first { $0.id == projectID && $0.available }
          store.newChat(workspace: project?.path ?? "")
          dismiss()
        }
        .buttonStyle(.borderedProminent)
        .disabled(store.busy || store.creatingChat || store.switchingModel || store.workspaceTransitioning || store.attachmentInteraction)
        .accessibilityIdentifier("recovery.newChat")
      }
    }
  }

  @ViewBuilder
  private func checkpointDetails(_ state: RecoveryState) -> some View {
    HStack {
      Text("恢复与未决结果").font(.title2.weight(.semibold))
      Spacer()
      Button("完成") { dismiss() }.disabled(busy)
    }
    GroupBox {
      VStack(alignment: .leading, spacing: 12) {
        Label(state.active ? "原回合尚未结束" : "当前没有活动回合",
              systemImage: state.active ? "clock.arrow.circlepath" : "checkmark.circle")
          .font(.headline)
        if !state.detail.isEmpty {
          Text(state.detail).font(.callout).foregroundStyle(.secondary)
        }
        if !state.pending.isEmpty {
          Text("未决调用 · \(state.pending)").font(.callout.monospaced())
        }
        if state.unknownCount > 0 {
          Text("此聊天保留了 \(state.unknownCount) 条结果未知的外部操作记录。")
            .foregroundStyle(.orange).font(.callout)
        }
      }.padding(12).frame(maxWidth: .infinity, alignment: .leading)
    }
    if !state.arguments.isEmpty {
      Text("当时批准的最终参数").font(.headline)
      ScrollView {
        Text(state.arguments).font(.system(.callout, design: .monospaced))
          .textSelection(.enabled).frame(maxWidth: .infinity, alignment: .leading)
      }.frame(maxHeight: 135)
    }
    if state.active {
      Text("继续会使用当时冻结的模型、提示词、预算和 Skills。未知工具调用会保持未决。")
        .font(.callout).foregroundStyle(.secondary)
      if !state.pending.isEmpty {
        Toggle("我理解外部效果可能已经发生，结束回合不会撤销或重试原调用", isOn: $acknowledge)
      } else if store.profileID.isEmpty {
        Text("原模型配置不可用，请恢复配置后继续。").foregroundStyle(.secondary)
      }
      HStack {
        if state.pending.isEmpty {
          Button("继续原回合") {
            guard store.selected == chatID else { return }
            dismiss()
            store.resume()
          }
          .buttonStyle(.borderedProminent)
          .disabled(busy || store.busy || store.profileID.isEmpty || !store.configurationReady)
        } else {
          Button("返回") { showsDetails = false }
        }
        Spacer()
        Button("结束回合并保留记录", role: .destructive) { abandon() }
          .disabled(busy || (!state.pending.isEmpty && !acknowledge))
      }
    }
  }

  private func reload() {
    guard !chatID.isEmpty else {
      error = "chat_not_found"
      return
    }
    store.workspaceRequest(["op": "recovery.inspect", "chat": chatID], as: RecoveryState.self) { reply in
      state = reply.data
      error = reply.error ?? ""
    }
  }

  private func abandon() {
    guard let state else { return }
    busy = true
    store.workspaceRequest([
      "op": "recovery.abandon", "chat": chatID, "stateHash": state.stateHash,
      "acknowledgeUnknown": acknowledge,
    ], as: RecoveryState.self) { reply in
      busy = false
      if reply.ok {
        self.state = reply.data
        error = ""
        store.reload()
        store.prepareIndex()
      } else {
        error = reply.error ?? "结束未完成"
      }
    }
  }

  private func message(_ code: String) -> String {
    switch code {
    case "chat_not_found": return "找不到这段聊天。"
    case "recovery_state_changed": return "回合状态已变化，请关闭后重新查看。"
    case "invalid_recovery_checkpoint", "invalid_recovery_pairing": return "恢复记录不完整，原数据仍然保留。"
    case "unknown_acknowledgement_required": return "请先确认对未知外部效果的说明。"
    default: return code
    }
  }
}
