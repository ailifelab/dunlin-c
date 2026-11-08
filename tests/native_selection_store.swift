import CDesktop
import Foundation
import Darwin

private struct CheckFailure: Error, CustomStringConvertible { let description: String }
@MainActor private func check(_ condition: Bool, _ message: String) throws {
  if !condition { throw CheckFailure(description: message) }
}
@MainActor private func waitUntil(_ message: String, _ condition: () -> Bool) async throws {
  let deadline = Date().addingTimeInterval(10)
  while !condition() {
    if Date() >= deadline { throw CheckFailure(description: "timed out: " + message) }
    try await Task.sleep(nanoseconds: 10_000_000)
  }
}
@main private struct NativeSelectionStoreCheck {
  @MainActor static func main() async {
    do { try await run(); print("PASS actual Swift store draft isolation, duplicate/stale selection events, same-chat routing and busy guards; no native window/gesture claim") }
    catch { print("FAIL \(error)"); exit(1) }
  }
  @MainActor static func run() async throws {
    let args = ProcessInfo.processInfo.arguments
    guard let index = args.firstIndex(of: "--data-dir"), index + 1 < args.count else {
      throw CheckFailure(description: "explicit owned --data-dir required")
    }
    let directory = URL(fileURLWithPath: args[index + 1], isDirectory: true)
    try check(directory.path.contains("/build/v51/native-selection-"), "refuse non-fixture state")
    try check(!FileManager.default.fileExists(atPath: directory.path), "refuse to reuse an existing fixture")
    let store = WorkspaceStore()
    try check(store.rootDirectory.standardizedFileURL == directory.standardizedFileURL, "unexpected data root")
    try await waitUntil("isolated bootstrap") { store.handle != nil && store.configurationReady && store.chatCatalogLoaded }
    guard let handle = store.handle else { throw CheckFailure(description: "missing isolated C store") }
    let project = directory.deletingLastPathComponent().appendingPathComponent("selection-files-" + UUID().uuidString)
    try FileManager.default.createDirectory(at: project, withIntermediateDirectories: false, attributes: [.posixPermissions: 0o700])
    let writer = store.queue
    let created: Bool = await withCheckedContinuation { continuation in
      writer.async {
        let a = dc_add_chat(handle.raw, "a", "Chat A", project.path)
        let b = dc_add_chat(handle.raw, "b", "Chat B", project.path)
        continuation.resume(returning: a != 0 && b != 0)
      }
    }
    try check(created, "fixture chat creation")
    store.reload()
    try await waitUntil("fixture chat catalog") { store.chats.count == 2 }
    store.selected = "a"
    store.selectChat(from: nil, to: "a")
    try await waitUntil("initial source load") { !store.messagesLoading }
    store.draft = "A editor draft"
    store.drafts["b"] = "B cached draft"
    store.selected = "b"
    // Two MainView observers can report the same shared selection transition.
    // Exercise the real store and durable C draft writer, not a model of it.
    store.selectChat(from: "a", to: "b")
    store.selectChat(from: "a", to: "b")
    try await waitUntil("new source load") { !store.messagesLoading }
    let draft: String? = await withCheckedContinuation { continuation in
      writer.async {
        let pointer = dc_workspace_request(handle.raw, "{\"op\":\"chats.metadata\",\"id\":\"a\"}")
        let reply = store.decode(pointer, as: WorkspaceReply<ChatMetadata>.self)
        continuation.resume(returning: reply?.data?.draft)
      }
    }
    try check(store.drafts["a"] == "A editor draft" && draft == "A editor draft", "duplicate selection overwrote outgoing draft with incoming draft: \(draft ?? "nil")")
    try check(store.draft == "B cached draft", "incoming editor draft changed")
    store.route = "project"
    store.conversationPage = "tools"
    try check(store.openChat("b"), "same selection did not open")
    try check(store.route == "chat" && store.conversationPage == "chat" && store.draft == "B cached draft", "same-chat routing changed draft or retained tool page")
    store.openToolRecords()
    try check(store.route == "tools" && store.selected == "b" && store.draft == "B cached draft", "dedicated tools route lost selected chat or editor draft")
    try check(store.openChat("b") && store.route == "chat" && store.draft == "B cached draft", "return from dedicated tools route lost editor draft")
    try check(store.openChat("b", sequence: 0) && store.jumpToMessage == 0, "source navigation intent not set")
    store.selectChat(from: "b", to: "a") // delayed notification; selection is b
    try check(store.draft == "B cached draft" && store.selected == "b", "stale notification changed current editor")
    store.busy = true
    store.openToolRecords()
    try check(store.route == "tools" && store.selected == "b", "live read-only tools route unavailable")
    try check(!store.openChat("a") && store.selected == "b", "busy navigation admitted")
    store.busy = false
    store.route = "chat"
    store.workspacePlanning = true
    try check(!store.openChat("a") && store.selected == "b", "workspace planning navigation admitted")
    store.workspacePlanning = false
    try await waitUntil("same-chat source refresh") { !store.messagesLoading }
    let seeded: Bool = await withCheckedContinuation { continuation in
      writer.async {
        let pointer = dc_workspace_request(handle.raw, "{\"op\":\"chats.update\",\"id\":\"b\",\"field\":\"draft\",\"value\":\"Durable B for the incoming epoch\"}")
        let reply = store.decode(pointer, as: WorkspaceReply<NoData>.self)
        continuation.resume(returning: reply?.ok == true)
      }
    }
    try check(seeded, "epoch fixture draft seed")
    // Identity switching resets the editor/cache before view notifications.
    // A previous epoch must not persist its outgoing selection into this one.
    store.identityEpoch += 1
    store.drafts = ["a": "Incoming epoch A editor"]
    store.draft = ""
    store.selected = "a"
    store.selectChat(from: "b", to: "a")
    store.selectChat(from: "b", to: "a")
    try await waitUntil("incoming epoch source refresh") { !store.messagesLoading }
    let otherDraft: String? = await withCheckedContinuation { continuation in
      writer.async {
        let pointer = dc_workspace_request(handle.raw, "{\"op\":\"chats.metadata\",\"id\":\"b\"}")
        continuation.resume(returning: store.decode(pointer, as: WorkspaceReply<ChatMetadata>.self)?.data?.draft)
      }
    }
    try check(otherDraft == "Durable B for the incoming epoch" && store.draft == "Incoming epoch A editor", "prior epoch notification overwrote an incoming-epoch draft")
  }
}
