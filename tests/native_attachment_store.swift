import CDesktop
import Foundation
import Darwin

private struct Failure: Error, CustomStringConvertible { let description: String }
@MainActor private func check(_ condition: Bool, _ message: String) throws {
  if !condition { throw Failure(description: message) }
}
@MainActor private func waitFor(_ message: String, _ predicate: () -> Bool) async throws {
  let deadline = Date().addingTimeInterval(15)
  while !predicate() {
    if Date() >= deadline { throw Failure(description: "timed out: " + message) }
    try await Task.sleep(nanoseconds: 10_000_000)
  }
}
@main private struct NativeAttachmentStoreCheck {
  @MainActor static func main() async {
    do { try await run(); print("PASS actual Swift/C attachment bridge: preview confirmation, durable input failure boundary, rejection/draft restoration, removal/resend and stale selection; no native gesture/capture claim") }
    catch { print("FAIL \(error)"); exit(1) }
  }
  @MainActor static func run() async throws {
    let args = ProcessInfo.processInfo.arguments
    guard let index = args.firstIndex(of: "--data-dir"), index + 1 < args.count,
      let endpointIndex = args.firstIndex(of: "--fixture-endpoint"), endpointIndex + 1 < args.count,
      let imageIndex = args.firstIndex(of: "--fixture-png"), imageIndex + 1 < args.count
    else { throw Failure(description: "explicit owned data/endpoint/PNG required") }
    let directory = URL(fileURLWithPath: args[index + 1], isDirectory: true)
    try check(directory.path.contains("/build/v51/native-attachment-"), "refuse non-fixture root")
    try check(!FileManager.default.fileExists(atPath: directory.path), "refuse reused state")
    try check(args[endpointIndex + 1].hasPrefix("http://127.0.0.1:"), "refuse external endpoint")
    let image = try Data(contentsOf: URL(fileURLWithPath: args[imageIndex + 1]))
    let store = WorkspaceStore()
    try check(store.rootDirectory.standardizedFileURL == directory.standardizedFileURL, "wrong data root")
    try await waitFor("bootstrap") { store.handle != nil && store.configurationReady && store.chatCatalogLoaded }
    guard let handle = store.handle else { throw Failure(description: "missing C handle") }
    let writer = store.queue
    func request<T: Decodable>(_ body: [String: Any], as type: T.Type) async -> WorkspaceReply<T>? {
      let raw = String(data: (try? JSONSerialization.data(withJSONObject: body)) ?? Data(), encoding: .utf8) ?? "{}"
      return await withCheckedContinuation { continuation in
        writer.async { continuation.resume(returning: store.decode(dc_workspace_request(handle.raw, raw), as: WorkspaceReply<T>.self)) }
      }
    }
    var model = ModelProfile()
    model.id = "fixture"
    model.name = "Owned native bridge fixture"
    model.endpoint = args[endpointIndex + 1]
    model.model = "fixture"
    model.authentication = "none"
    model.maxOutput = 256
    model.inputModalities = ["text"]
    let encoded = try JSONSerialization.jsonObject(with: JSONEncoder().encode([model]))
    let registry = await request(["op": "config.save", "revision": store.configRevision,
      "models": encoded, "providers": []], as: ConfigRegistry.self)
    try check(registry?.ok == true && registry?.data != nil, "model registry fixture")
    store.acceptConfiguration(registry!.data!)
    store.defaultProfileID = model.id
    store.profileID = model.id
    let work = directory.deletingLastPathComponent().appendingPathComponent("files", isDirectory: true)
    try FileManager.default.createDirectory(at: work, withIntermediateDirectories: false, attributes: [.posixPermissions: 0o700])
    let file = work.appendingPathComponent("资料.txt")
    try "不要删除 A；只修改 B。\r\n".write(to: file, atomically: true, encoding: .utf8)
    let created: Bool = await withCheckedContinuation { continuation in
      writer.async { continuation.resume(returning: dc_add_chat(handle.raw, "a", "A", work.path) != 0 && dc_add_chat(handle.raw, "b", "B", work.path) != 0) }
    }
    try check(created, "fixture chats")
    for chat in ["a", "b"] {
      let reply = await request(["op": "chats.update", "id": chat, "field": "model", "value": model.id], as: NoData.self)
      try check(reply?.ok == true, "fixture model assignment")
    }
    store.reload()
    try await waitFor("catalog") { store.chats.count == 2 }
    store.selected = "a"; store.selectChat(from: nil, to: "a")
    try await waitFor("A selection") { !store.messagesLoading && store.attachmentsLoaded }
    store.previewAttachmentFile(file)
    try await waitFor("file preview") { store.attachmentSelection != nil && !store.attachmentBusy }
    let pendingBefore = await request(["op": "attachments.pending", "chat": "a"], as: ChatAttachment.self)
    try check(pendingBefore?.ok == true && pendingBefore?.data == nil, "preview persisted before confirmation")
    store.addSelectedAttachment(store.attachmentSelection!)
    try await waitFor("confirmed pending") { store.attachmentSelection == nil && store.pendingAttachment != nil && !store.attachmentBusy }
    try check(store.canSendMessage, "attachment-only input unavailable")
    store.draft = "发送后网络失败"
    store.send()
    try check(store.busy, "send did not start")
    try await waitFor("HTTP failure settlement") { !store.busy && !store.messagesLoading && store.attachmentsLoaded }
    try check(store.draft.isEmpty && store.pendingAttachment == nil, "accepted failed input restored as an unsent duplicate")
    try check(store.messages.filter { $0.role == "user" }.map(\.content) == ["发送后网络失败"], "accepted source not projected")
    let afterHTTP = await request(["op": "chats.metadata", "id": "a"], as: ChatMetadata.self)
    try check(afterHTTP?.data?.draft == "", "accepted source left old durable draft")
    store.previewAttachmentBytes(image, name: "pending.png", origin: "clipboard")
    try await waitFor("image preview") { store.attachmentSelection != nil && !store.attachmentBusy }
    store.addSelectedAttachment(store.attachmentSelection!)
    try await waitFor("image confirmed") { store.pendingAttachment?.format == "png" && !store.attachmentBusy }
    let id = store.pendingAttachment!.id
    store.draft = "拒绝图片也保留我"
    store.send()
    try await waitFor("unsupported image admission") { !store.busy && !store.messagesLoading && store.attachmentsLoaded }
    try check(store.draft == "拒绝图片也保留我" && store.pendingAttachment?.id == id, "rejected admission lost draft or original pending selection")
    try check(store.messages.filter { $0.role == "user" }.count == 1, "rejected input committed")
    let rejected = await request(["op": "chats.metadata", "id": "a"], as: ChatMetadata.self)
    try check(rejected?.data?.draft == "拒绝图片也保留我", "rejected durable draft lost")
    store.removePendingAttachment()
    try await waitFor("pending removal") { !store.attachmentBusy && store.pendingAttachment == nil }
    try check(store.canSendMessage, "restored draft cannot be sent after removing incompatible image")
    store.send()
    try await waitFor("successful follow-up") { !store.busy && !store.messagesLoading && store.attachmentsLoaded }
    try check(store.draft.isEmpty && store.messages.filter { $0.role == "user" }.map(\.content) == ["发送后网络失败", "拒绝图片也保留我"], "follow-up duplicated old source or retained sent draft")
    // Delayed C file read from A must not install a candidate into B. This
    // exercises an actual outstanding queue operation and shared store epoch,
    // rather than reimplementing a generation predicate in the test.
    let gate = DispatchSemaphore(value: 0)
    writer.async { gate.wait() }
    store.previewAttachmentFile(file)
    try check(store.attachmentBusy, "preview operation was not enqueued")
    store.selected = "b"; store.selectChat(from: "a", to: "b")
    gate.signal()
    try await waitFor("B selection after delayed A read") { !store.messagesLoading && store.attachmentsLoaded }
    let barrier: Bool = await withCheckedContinuation { c in writer.async { c.resume(returning: true) } }
    try check(barrier && store.attachmentSelection == nil && store.pendingAttachment == nil && store.messages.isEmpty, "delayed A attachment leaked into B")
  }
}
