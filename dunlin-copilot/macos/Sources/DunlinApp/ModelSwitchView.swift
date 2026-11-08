import CDesktop
import SwiftUI

struct ModelSwitchReview: Decodable, Identifiable {
  var id: String { chat + ":" + targetID + ":" + historyHash + ":" + String(configRevision) + ":" + (imagePolicy ?? "images") + ":" + (pendingID ?? "") }
  let chat: String
  let targetID: String
  let targetName: String
  let currentID: String
  let currentName: String
  let `protocol`: String
  let configRevision: Int64
  let historyHash: String
  let messageCount: Int
  let toolResults: Int
  let fileReceipts: Int
  let imageCount: Int
  let pendingImageCount: Int?
  let pendingID: String?
  let imagePolicy: String?
  let currentImagePolicy: String?
  let canTextOnly: Bool?
  let canSwitch: Bool
  let reason: String?
  let contextKnown: Bool
  let contextSelected: Int?
  let contextFit: String
}
struct ModelSwitchView: View {
  @EnvironmentObject var store: WorkspaceStore
  @Environment(\.openSettings) private var openSettings
  let review: ModelSwitchReview
  var body: some View {
    VStack(alignment: .leading, spacing: 26) {
      Text("切换模型，继续这段聊天").font(.system(size: 23, weight: .semibold))
      Text("聊天记录和已执行操作将完整保留。").font(.callout).foregroundStyle(.secondary)
      VStack(alignment: .leading, spacing: 0) {
        modelRow(review.currentName, detail: "当前模型")
        Divider()
        modelRow(review.targetName, detail: "目标模型 · \(protocolName)")
      }.padding(.horizontal, 18).background(
        Color(nsColor: .textBackgroundColor), in: RoundedRectangle(cornerRadius: 12))
      VStack(alignment: .leading, spacing: 14) {
        Label(
          review.canSwitch ? "上下文保留检查通过" : "需要确认模型能力",
          systemImage: review.canSwitch ? "checkmark.circle" : "exclamationmark.triangle"
        )
        .font(.system(size: 17, weight: .semibold))
        Text(
          "将保留 \(review.messageCount) 条消息、\(review.fileReceipts) 次文件操作回执与 \(review.toolResults) 次工具结果。"
        )
        Text("服务商专属的续接状态不会传给另一家服务商。")
        if review.canTextOnly == true {
          Toggle("仅传文字，原图保留", isOn: Binding(
            get: { review.imagePolicy == "textOnly" },
            set: { store.previewModelSwitch(to: review.targetID, imagePolicy: $0 ? "textOnly" : "images", allowSame: true) }
          )).toggleStyle(.checkbox).disabled(store.switchingModel)
            .accessibilityIdentifier("model.switch.textOnly")
          Text(review.imagePolicy == "textOnly"
            ? "历史图片 \(review.imageCount) 张仍可回看；下一回合只发送文字和图片引用，不推断未传入的画面。"
            : "历史图片 \(review.imageCount) 张。图片占用按服务商计量，文字预算估计不代表完整图片令牌占用。")
          if (review.pendingImageCount ?? 0) > 0 {
            Text("另有一张待发图片；切换后仍会保留。发送新图片需要可用的图片模型和图片传递方式。")
          }
        }
        if review.canSwitch {
          Text(
            review.contextKnown
              ? "目标上下文预算 \(review.contextSelected ?? 0)；发送前会按配置检查并压缩上下文。"
              : "目标模型的窗口尚未确认，可在模型设置中补充。历史记录不会清空。")
        } else {
          Text(modelSwitchExplanation(review.reason))
        }
      }.font(.system(size: 14)).foregroundStyle(review.canSwitch ? Color.accentColor : Color.orange)
        .padding(20).frame(maxWidth: .infinity, alignment: .leading)
        .background(
          (review.canSwitch ? Color.accentColor : Color.orange).opacity(0.07),
          in: RoundedRectangle(cornerRadius: 12))
      Text("切换只影响下一回合；不会重跑历史工具。").font(.callout).foregroundStyle(.secondary)
      if !store.modelSwitchError.isEmpty {
        Text(store.modelSwitchError).foregroundStyle(.red).font(.callout).accessibilityIdentifier(
          "model.switch.error")
      }
      HStack {
        if !review.canSwitch {
          Button("模型设置…") {
            store.cancelModelSwitch()
            store.settingsSection = "模型"
            openSettings()
          }
        } else if !store.modelSwitchError.isEmpty {
          Button("重新检查") { store.previewModelSwitch(to: review.targetID) }
        }
        Spacer()
        Button("取消", action: store.cancelModelSwitch).keyboardShortcut(.cancelAction)
          .accessibilityIdentifier("model.switch.cancel")
        Button(store.switchingModel ? "正在切换…" : "切换并继续聊天") { store.applyModelSwitch(review) }
          .buttonStyle(.borderedProminent).disabled(!review.canSwitch || store.switchingModel)
          .keyboardShortcut(.defaultAction)
          .accessibilityIdentifier("model.switch.apply")
      }.disabled(store.switchingModel)
    }.padding(32).frame(width: 760).interactiveDismissDisabled(store.switchingModel)
      .accessibilityElement(children: .contain).accessibilityIdentifier("model.switch.review")
  }
  private var protocolName: String {
    switch review.protocol {
    case "chat": return "Chat"
    case "responses": return "Responses"
    case "anthropic": return "Messages"
    case "gemini": return "Gemini"
    case "ollama": return "Ollama"
    default: return review.protocol
    }
  }
  private func modelRow(_ name: String, detail: String) -> some View {
    VStack(alignment: .leading, spacing: 6) {
      Text(name).font(.system(size: 16, weight: .medium))
      Text(detail).font(.system(size: 12)).foregroundStyle(.secondary)
    }.padding(.vertical, 18).frame(maxWidth: .infinity, alignment: .leading)
  }
}
func modelSwitchExplanation(_ error: String?) -> String {
  switch error {
  case "model_switch_configuration_changed": return "模型配置已变化，请重新检查后切换。"
  case "model_switch_history_changed": return "聊天或当前模型已变化，请重新检查；原始记录保留。"
  case "model_switch_active_checkpoint": return "当前回合尚未结算，请先恢复或结束该回合。"
  case "model_switch_history_invalid": return "聊天检查点暂不可读，请先核对恢复状态。原始数据保留。"
  case "model_switch_model_missing": return "目标模型已移除，请选择其他模型。"
  case "model_switch_model_unusable": return "目标模型参数需要修复，请先打开模型设置。"
  case "model_switch_image_capability_unconfirmed": return "聊天或待发内容包含图片，目标模型尚未声明支持图片。请确认模型能力或选择支持图片的模型。"
  case "model_switch_image_protocol_unsupported": return "当前协议适配不支持这段历史中的图片，请选择支持图片的模型。"
  case "model_switch_attachment_source_invalid": return "附件来源校验失败，原聊天与原图保留，请先检查本地附件。"
  case "model_switch_legacy_image_projection_unavailable": return "旧版内嵌图片尚不能转换为仅文字投影，请选择可用的图片模型。"
  case "model_switch_chat_missing": return "聊天已不存在，请返回聊天列表。"
  default: return "切换未完成，原模型与聊天记录保留。请稍后重试。"
  }
}
extension WorkspaceStore {
  func cancelModelSwitch() {
    guard !switchingModel else { return }
    modelSwitchRequestID = UUID()
    modelSwitchPlan = nil
    modelSwitchError = ""
  }
  func previewModelSwitch(to target: String, imagePolicy: String = "images", allowSame: Bool = false) {
    guard !busy, !creatingChat, !switchingModel, !configSaving, !checkingConnection,
      configurationReady,
      profiles.contains(where: { $0.id == target }),
      !attachmentInteraction,
      target != profileID || selectedChat?.model != target || allowSame
    else { return }
    guard let chat = selected, let h = handle else {
      profileID = target
      saveSettings()
      return
    }
    let epoch = identityEpoch
    let request = UUID()
    modelSwitchRequestID = request
    modelSwitchError = ""
    switchingModel = true
    let body: [String: Any] = [
      "op": "chats.model.preview", "chat": chat, "target": target, "revision": configRevision,
      "imagePolicy": imagePolicy,
    ]
    guard let data = try? JSONSerialization.data(withJSONObject: body),
      let raw = String(data: data, encoding: .utf8)
    else {
      switchingModel = false
      return
    }
    queue.async {
      let reply = self.decode(
        dc_workspace_request(h.raw, raw), as: WorkspaceReply<ModelSwitchReview>.self)
      DispatchQueue.main.async {
        guard self.identityEpoch == epoch, self.modelSwitchRequestID == request else { return }
        self.switchingModel = false
        guard self.selected == chat else { return }
        if let reply, reply.ok, let review = reply.data {
          self.modelSwitchPlan = review
        } else {
          let message = modelSwitchExplanation(reply?.error)
          if self.modelSwitchPlan == nil {
            self.error = message
          } else {
            self.modelSwitchError = message
          }
        }
      }
    }
  }
  func applyModelSwitch(_ review: ModelSwitchReview) {
    guard !busy, !switchingModel, !configSaving, !checkingConnection,
      selected == review.chat, review.canSwitch, let h = handle
    else { return }
    let epoch = identityEpoch
    let request = UUID()
    modelSwitchRequestID = request
    switchingModel = true
    modelSwitchError = ""
    let body: [String: Any] = [
      "op": "chats.model.apply", "chat": review.chat, "target": review.targetID,
      "revision": review.configRevision, "currentID": review.currentID,
      "historyHash": review.historyHash,
      "pendingID": review.pendingID ?? "", "imagePolicy": review.imagePolicy ?? "images",
      "currentImagePolicy": review.currentImagePolicy ?? "images",
    ]
    guard let data = try? JSONSerialization.data(withJSONObject: body),
      let raw = String(data: data, encoding: .utf8)
    else {
      switchingModel = false
      return
    }
    queue.async {
      let reply = self.decode(
        dc_workspace_request(h.raw, raw), as: WorkspaceReply<ModelSwitchReview>.self)
      DispatchQueue.main.async {
        guard self.identityEpoch == epoch, self.modelSwitchRequestID == request else { return }
        self.switchingModel = false
        if let reply, reply.ok {
          self.profileID = review.targetID
          self.modelSwitchPlan = nil
          self.saveSettings()
          self.reload()
        } else {
          self.modelSwitchError = modelSwitchExplanation(reply?.error)
        }
      }
    }
  }
}
