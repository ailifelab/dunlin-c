import AppKit
import ImageIO
import SwiftUI

struct AttachmentCard: View {
  @EnvironmentObject var store: WorkspaceStore
  let attachment: ChatAttachment
  let chat: String
  var pending = false
  @State private var image: NSImage?
  @State private var imageRequest = UUID()
  var body: some View {
    VStack(alignment: .leading, spacing: 10) {
      HStack(spacing: 12) {
        Button { store.viewAttachment(attachment, chat: chat) } label: {
          HStack(spacing: 12) {
            Image(systemName: attachment.format == "png" ? "photo" : "doc.text")
              .font(.system(size: 20)).foregroundStyle(.secondary)
            VStack(alignment: .leading, spacing: 4) {
              Text(attachment.name).font(.system(size: 13, weight: .medium)).lineLimit(2)
              Text(attachment.detail).font(.caption).foregroundStyle(.secondary)
            }
          }.frame(maxWidth: .infinity, alignment: .leading).contentShape(Rectangle())
        }.buttonStyle(.plain).disabled(store.attachmentBusy)
          .accessibilityIdentifier(pending ? "attachment.pending.preview" : "attachment.history.\(attachment.id)")
        if pending {
          Button { store.removePendingAttachment() } label: {
            Image(systemName: "xmark.circle.fill").foregroundStyle(.secondary)
          }.buttonStyle(.plain).help("移除本轮附件").disabled(!store.canEditAttachment)
            .accessibilityIdentifier("attachment.remove")
        }
      }
      if let image, !pending {
        Button { store.viewAttachment(attachment, chat: chat) } label: {
          Image(nsImage: image).resizable().scaledToFit().frame(maxWidth: 300, maxHeight: 220)
            .clipShape(RoundedRectangle(cornerRadius: 8))
        }.buttonStyle(.plain).accessibilityLabel("预览图片 \(attachment.name)")
      }
    }.padding(12).background(Color.primary.opacity(0.035), in: RoundedRectangle(cornerRadius: 10))
      .task(id: "\(store.identityEpoch):\(chat):\(attachment.id):\(pending)") {
        let request = UUID()
        imageRequest = request
        image = nil
        let epoch = store.identityEpoch
        guard attachment.format == "png", !pending else { return }
        store.readAttachment(attachment, chat: chat) { reply in
          guard imageRequest == request, store.identityEpoch == epoch,
            store.selected == chat, let data = reply.data?.png else { return }
          DispatchQueue.global(qos: .utility).async {
            let decoded = decodeAttachmentImage(data)
            DispatchQueue.main.async {
              guard imageRequest == request, store.identityEpoch == epoch, store.selected == chat else { return }
              self.image = decoded
            }
          }
        }
      }
      .onDisappear { imageRequest = UUID(); image = nil }
  }
}
struct AttachmentPreviewView: View {
  @EnvironmentObject var store: WorkspaceStore
  let selection: AttachmentSelection
  @State private var image: NSImage?
  @State private var decoding = true
  @State private var decodeError = false
  @State private var imageRequest = UUID()
  var body: some View {
    VStack(alignment: .leading, spacing: 22) {
      Text(selection.adding ? "添加本轮上下文" : "查看原始附件")
        .font(.system(size: 23, weight: .semibold))
      Text(selection.adding
        ? "这些内容将在下一条消息中发送给所选模型。"
        : "此处展示已保存的原始内容，切换模型不会删除附件。")
        .font(.callout).foregroundStyle(.secondary)
      VStack(alignment: .leading, spacing: 6) {
        Text(selection.payload.name).font(.system(size: 15, weight: .medium))
        Text(selection.payload.record.detail).font(.caption).foregroundStyle(.secondary)
        if selection.payload.kind == "screenshot" {
          Text("系统选取的窗口或屏幕 · 最长边 1024 像素 · SDR PNG")
            .font(.caption).foregroundStyle(.secondary)
        }
      }.padding(16).frame(maxWidth: .infinity, alignment: .leading)
        .background(Color.primary.opacity(0.035), in: RoundedRectangle(cornerRadius: 10))
      Text("内容预览").font(.system(size: 14, weight: .semibold))
      Group {
        if selection.payload.format == "png" {
          if let image {
            ScrollView([.horizontal, .vertical]) {
              Image(nsImage: image).resizable().scaledToFit().frame(maxWidth: 660, maxHeight: 340)
                .padding(14)
            }
          } else if decoding { ProgressView("正在读取图片…").frame(maxWidth: .infinity, maxHeight: .infinity) }
          else { ContentUnavailableView("系统无法显示这张 PNG", systemImage: "photo.badge.exclamationmark") }
        } else {
          ScrollView {
            Text(selection.payload.text?.isEmpty == false ? selection.payload.text! : "（空文本文件）")
              .font(.system(size: 13, design: .monospaced)).textSelection(.enabled)
              .frame(maxWidth: .infinity, alignment: .leading).padding(16)
          }
        }
      }.frame(height: 330).frame(maxWidth: .infinity)
        .background(Color(nsColor: .textBackgroundColor), in: RoundedRectangle(cornerRadius: 10))
        .overlay(RoundedRectangle(cornerRadius: 10).stroke(Color.primary.opacity(0.1), lineWidth: 1))
      if selection.adding {
        Text(selection.expectedID == nil
          ? "正文为本次读取快照；发送前可移除。不会静默裁剪超限内容。"
          : "添加后将替换当前待发附件。已发送的历史附件保持原样。")
          .font(.caption).foregroundStyle(.secondary)
      }
      if !store.attachmentError.isEmpty {
        Text(store.attachmentError).font(.callout).foregroundStyle(.red).textSelection(.enabled)
      }
      HStack {
        if !selection.adding, let png = selection.payload.png, !decodeError {
          Button("复制图片") {
            NSPasteboard.general.clearContents()
            NSPasteboard.general.setData(png, forType: .png)
          }.accessibilityIdentifier("attachment.copyImage")
        }
        Spacer()
        Button(selection.adding ? "取消" : "完成") { store.attachmentSelection = nil }
          .keyboardShortcut(.cancelAction).disabled(store.attachmentBusy)
          .accessibilityIdentifier("attachment.preview.cancel")
        if selection.adding {
          Button(store.attachmentBusy ? "正在添加…" : "添加上下文") { store.addSelectedAttachment(selection) }
            .buttonStyle(.borderedProminent).keyboardShortcut(.defaultAction)
            .disabled(store.attachmentBusy || (selection.payload.format == "png" && (decoding || decodeError)))
            .accessibilityIdentifier("attachment.preview.add")
        }
      }
    }.padding(32).frame(width: 760).interactiveDismissDisabled(store.attachmentBusy)
      .accessibilityElement(children: .contain).accessibilityIdentifier("attachment.preview")
      .task(id: selection.id) {
        let request = UUID()
        imageRequest = request
        image = nil; decoding = true; decodeError = false
        guard selection.payload.format == "png", let data = selection.payload.png else {
          decoding = false
          return
        }
        DispatchQueue.global(qos: .utility).async {
          let decoded = decodeAttachmentImage(data)
          DispatchQueue.main.async {
            guard imageRequest == request, store.identityEpoch == selection.epoch,
              store.selected == selection.chat, store.attachmentSelection?.id == selection.id else { return }
            image = decoded; decoding = false; decodeError = decoded == nil
          }
        }
      }
      .onDisappear { imageRequest = UUID() }
  }
}
private func decodeAttachmentImage(_ data: Data) -> NSImage? {
  guard data.count <= 1_048_576, let source = CGImageSourceCreateWithData(data as CFData, nil),
    let cg = CGImageSourceCreateImageAtIndex(source, 0, nil), cg.width <= 1024, cg.height <= 1024
  else { return nil }
  return NSImage(cgImage: cg, size: NSSize(width: cg.width, height: cg.height))
}
