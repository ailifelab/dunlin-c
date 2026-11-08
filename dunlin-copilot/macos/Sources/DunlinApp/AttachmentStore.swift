import AppKit
import CDesktop
import SwiftUI
import UniformTypeIdentifiers

struct ChatAttachment: Codable, Identifiable, Equatable {
  let id: String
  let name: String
  let kind: String
  let sha256: String
  let format: String
  let mime: String
  let bytes: Int
  let width: Int
  let height: Int
  var canonicalHash: String?
  var sequence: Int?
  var detail: String {
    let size = ByteCountFormatter.string(fromByteCount: Int64(bytes), countStyle: .file)
    return format == "png" ? "PNG · \(width) × \(height) · \(size)" : "UTF-8 文本 · \(size)"
  }
}
struct AttachmentPayload: Decodable {
  let id: String
  let name: String
  let kind: String
  let sha256: String
  let format: String
  let mime: String
  let bytes: Int
  let width: Int
  let height: Int
  let dataURL: String?
  let text: String?
  var record: ChatAttachment {
    ChatAttachment(id: id, name: name, kind: kind, sha256: sha256, format: format,
      mime: mime, bytes: bytes, width: width, height: height)
  }
  var png: Data? {
    guard format == "png", let url = dataURL, url.hasPrefix("data:image/png;base64,") else { return nil }
    return Data(base64Encoded: String(url.dropFirst(22)))
  }
}
struct AttachmentSelection: Identifiable {
  let id = UUID()
  let chat: String
  let epoch: Int64
  let payload: AttachmentPayload
  let file: URL?
  let bytes: Data?
  let expectedID: String?
  let adding: Bool
}
extension WorkspaceStore {
  var attachmentInteraction: Bool {
    attachmentBusy || attachmentSelection != nil || screenshotCapture != nil
  }
  var canEditAttachment: Bool {
    !busy && !creatingChat && !switchingModel && !attachmentInteraction
      && !workspaceTransitioning && attachmentsLoaded && selected != nil && selectedChat?.active != true
  }
  func resetAttachments() {
    attachmentRequestID = UUID()
    pendingAttachment = nil
    attachmentsLoaded = false
    attachmentSelection = nil
    attachmentBusy = false
    attachmentError = ""
    screenshotCapture?.cancel()
    screenshotCapture = nil
  }
  func reloadPendingAttachment() {
    let request = UUID()
    attachmentRequestID = request
    guard let chat = selected else { attachmentsLoaded = false; return }
    attachmentsLoaded = false
    readLocalView(["op": "attachments.pending", "chat": chat], as: ChatAttachment.self) { reply in
      guard self.selected == chat, self.attachmentRequestID == request else { return }
      self.attachmentsLoaded = reply.ok
      if reply.ok {
        self.pendingAttachment = reply.data
        self.attachmentError = ""
      } else {
        self.attachmentError = self.attachmentMessage(reply.error)
      }
    }
  }
  func chooseAttachment() {
    guard canEditAttachment else { return }
    let panel = NSOpenPanel()
    panel.canChooseFiles = true
    panel.canChooseDirectories = false
    panel.allowsMultipleSelection = false
    panel.allowedContentTypes = [.png, .plainText, .sourceCode, .json, .xml, .commaSeparatedText]
    panel.message = "选择 PNG 图片或 UTF-8 文本；预览并确认后加入本轮上下文。"
    panel.prompt = "预览"
    guard panel.runModal() == .OK, let url = panel.url else { return }
    previewAttachmentFile(url)
  }
  func previewAttachmentFile(_ url: URL) {
    guard canEditAttachment, let chat = selected, let h = handle else { return }
    let format = url.pathExtension.lowercased() == "png" ? "png" : "text"
    let epoch = identityEpoch
    let expected = pendingAttachment?.id
    let request = UUID()
    attachmentRequestID = request
    attachmentBusy = true
    attachmentError = ""
    let body: [String: Any] = ["op": "attachments.previewFile", "chat": chat,
      "path": url.path, "format": format]
    guard let data = try? JSONSerialization.data(withJSONObject: body),
      let raw = String(data: data, encoding: .utf8) else { attachmentBusy = false; return }
    queue.async {
      let reply = self.decode(dc_workspace_request(h.raw, raw), as: WorkspaceReply<AttachmentPayload>.self)
      DispatchQueue.main.async {
        guard self.identityEpoch == epoch, self.selected == chat, self.attachmentRequestID == request else { return }
        self.attachmentBusy = false
        if let reply, reply.ok, let payload = reply.data {
          self.attachmentSelection = AttachmentSelection(chat: chat, epoch: epoch, payload: payload,
            file: url, bytes: nil, expectedID: expected, adding: true)
        } else { self.attachmentError = self.attachmentMessage(reply?.error) }
      }
    }
  }
  func pasteAttachment(_ data: Data) {
    guard canEditAttachment else { return }
    previewAttachmentBytes(data, name: "剪贴板图片.png", origin: "clipboard")
  }
  func previewAttachmentBytes(_ data: Data, name: String, origin: String) {
    guard canEditAttachment, let chat = selected, handle != nil else { return }
    let epoch = identityEpoch
    let expected = pendingAttachment?.id
    let request = UUID()
    attachmentRequestID = request
    attachmentBusy = true
    attachmentError = ""
    queue.async {
      let reply = data.withUnsafeBytes { buffer in
        self.decode(dc_attachment_preview(name, origin, 1, buffer.baseAddress, buffer.count),
          as: WorkspaceReply<AttachmentPayload>.self)
      }
      DispatchQueue.main.async {
        guard self.identityEpoch == epoch, self.selected == chat, self.attachmentRequestID == request else { return }
        self.attachmentBusy = false
        if let reply, reply.ok, let payload = reply.data {
          self.attachmentSelection = AttachmentSelection(chat: chat, epoch: epoch, payload: payload,
            file: nil, bytes: data, expectedID: expected, adding: true)
        } else { self.attachmentError = self.attachmentMessage(reply?.error) }
      }
    }
  }
  func addSelectedAttachment(_ selection: AttachmentSelection) {
    guard !busy, !attachmentBusy, attachmentSelection?.id == selection.id,
      selected == selection.chat, identityEpoch == selection.epoch, selection.adding,
      let h = handle else { return }
    attachmentBusy = true
    attachmentError = ""
    queue.async {
      let raw: UnsafeMutablePointer<CChar>?
      if let url = selection.file {
        var body: [String: Any] = ["op": "attachments.stageFile", "chat": selection.chat,
          "path": url.path, "format": selection.payload.format, "expectedSHA": selection.payload.sha256]
        if let expected = selection.expectedID { body["expectedID"] = expected }
        let source = String(data: (try? JSONSerialization.data(withJSONObject: body)) ?? Data(), encoding: .utf8) ?? "{}"
        raw = dc_workspace_request(h.raw, source)
      } else if let bytes = selection.bytes {
        raw = bytes.withUnsafeBytes { buffer in
          if let expected = selection.expectedID {
            return expected.withCString { dc_attachment_stage(h.raw, selection.chat, $0,
              selection.payload.name, selection.payload.kind, 1, buffer.baseAddress, buffer.count) }
          }
          return dc_attachment_stage(h.raw, selection.chat, nil, selection.payload.name,
            selection.payload.kind, 1, buffer.baseAddress, buffer.count)
        }
      } else { raw = nil }
      let result = self.decode(raw, as: WorkspaceReply<ChatAttachment>.self)
      DispatchQueue.main.async {
        guard self.identityEpoch == selection.epoch, self.selected == selection.chat,
          self.attachmentSelection?.id == selection.id else { return }
        self.attachmentBusy = false
        if let result, result.ok, let attachment = result.data {
          self.pendingAttachment = attachment
          self.attachmentsLoaded = true
          self.attachmentSelection = nil
        } else { self.attachmentError = self.attachmentMessage(result?.error) }
      }
    }
  }
  func removePendingAttachment() {
    guard canEditAttachment, let chat = selected, let attachment = pendingAttachment else { return }
    attachmentBusy = true
    workspaceRequest(["op": "attachments.remove", "chat": chat, "expectedID": attachment.id], as: NoData.self) { result in
      guard self.selected == chat else { return }
      self.attachmentBusy = false
      if result.ok { self.pendingAttachment = nil; self.attachmentError = "" }
      else { self.attachmentError = self.attachmentMessage(result.error); self.reloadPendingAttachment() }
    }
  }
  func viewAttachment(_ attachment: ChatAttachment, chat: String) {
    guard !attachmentBusy, attachmentSelection == nil, selected == chat else { return }
    attachmentBusy = true
    attachmentError = ""
    let epoch = identityEpoch
    readAttachment(attachment, chat: chat) { reply in
      guard self.identityEpoch == epoch, self.selected == chat else { return }
      self.attachmentBusy = false
      if reply.ok, let payload = reply.data {
        self.attachmentSelection = AttachmentSelection(chat: chat, epoch: epoch, payload: payload,
          file: nil, bytes: nil, expectedID: nil, adding: false)
      } else { self.attachmentError = self.attachmentMessage(reply.error) }
    }
  }
  func readAttachment(_ attachment: ChatAttachment, chat: String,
    completion: @escaping (WorkspaceReply<AttachmentPayload>) -> Void) {
    var body: [String: Any] = ["op": "attachments.read", "chat": chat]
    if let seq = attachment.sequence, let hash = attachment.canonicalHash {
      body["sequence"] = seq; body["canonicalHash"] = hash
    } else { body["draftID"] = attachment.id }
    readLocalView(body, as: AttachmentPayload.self, completion: completion)
  }
  func captureScreenshot() {
    guard canEditAttachment else { return }
    let epoch = identityEpoch
    let chat = selected
    attachmentError = ""
    let capture = ScreenshotCapture { [weak self] result in
      guard let self, self.identityEpoch == epoch, self.selected == chat else { return }
      self.screenshotCapture = nil
      switch result {
      case .success(let data): self.previewAttachmentBytes(data, name: "截图.png", origin: "screenshot")
      case .failure(let error): self.attachmentError = error.localizedDescription
      case .none: break
      }
    }
    screenshotCapture = capture
    capture.start()
  }
  func attachmentMessage(_ error: String?) -> String {
    switch error {
    case "attachment_size_or_quota_limit", "attachment_file_type_or_size_limit":
      return "PNG 最多 1 MiB、最长边 1024 像素；UTF-8 文本最多 64 KiB。内容不会被静默裁剪。"
    case "attachment_file_changed_since_preview", "attachment_file_changed_during_read":
      return "文件在预览后发生变化，请重新选择并检查。原待发附件保留。"
    case "attachment_selection_changed": return "待发附件已变化，请重新读取后操作。"
    case "attachment_active_checkpoint": return "请先恢复或结束当前回合，再修改待发附件。"
    case "attachment_source_changed", "attachment_manifest_invalid": return "附件来源已变化，无法显示；原始数据保留。"
    case "attachment_upgrade_backup_or_schema_failed": return "无法完成附件数据升级或保存升级前副本，原聊天保留。"
    case "attachment_invalid_format_or_name", "attachment_payload_invalid": return "文件格式不可用。请选择静态 PNG 或不含 NUL 的 UTF-8 文本。"
    case "attachment_file_unreadable": return "无法读取所选文件，请检查位置与读取权限。"
    default: return "附件操作未完成：" + (error ?? "无法读取本地数据")
    }
  }
}
