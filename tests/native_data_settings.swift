import CDesktop
import Foundation

private struct Failure: Error { let message: String }
@MainActor private func require(_ condition: Bool, _ message: String) throws {
  if !condition { throw Failure(message: message) }
}

@main private struct DataSettingsCheck {
  @MainActor static func main() async throws {
    let args = ProcessInfo.processInfo.arguments
    guard let index = args.firstIndex(of: "--data-dir"), args.count > index + 1 else {
      throw Failure(message: "explicit fixture directory required")
    }
    let root = URL(fileURLWithPath: args[index+1]).standardizedFileURL
    try require(root.path.contains("/build/v51/privacy-layout/"), "isolated directory required")
    try require(!FileManager.default.fileExists(atPath: root.path), "fresh fixture required")
    let store = WorkspaceStore()
    func wait(_ condition: () -> Bool) async throws {
      let deadline = Date().addingTimeInterval(10)
      while !condition() {
        try require(Date() < deadline, "fixture timed out")
        try await Task.sleep(nanoseconds: 20_000_000)
      }
    }
    try await wait { store.configurationReady && store.chatCatalogLoaded }
    guard let handle = store.handle else { throw Failure(message: "missing store") }
    let project = root.deletingLastPathComponent().appendingPathComponent("files-"+UUID().uuidString)
    try FileManager.default.createDirectory(at: project, withIntermediateDirectories: false)
    let file = project.appendingPathComponent("untouched.txt")
    try "project content".write(to: file, atomically: true, encoding: .utf8)
    let created = await withCheckedContinuation { continuation in store.queue.async {
      let a = dc_add_chat(handle.raw, "a", "Active list item", project.path)
      let b = dc_add_chat(handle.raw, "b", "Archived item", project.path)
      let reply = store.decode(dc_workspace_request(handle.raw,"{\"op\":\"chats.update\",\"id\":\"b\",\"field\":\"archived\",\"value\":true}"), as: WorkspaceReply<NoData>.self)
      continuation.resume(returning: a != 0 && b != 0 && reply?.ok == true)
    } }
    try require(created, "failed to seed chats")
    store.reload()
    try await wait { store.chats.count == 2 }
    let reviewed = store.chats
    try require(reviewed.contains(where: \.archived), "archive not present")
    let addedLater = await withCheckedContinuation { continuation in store.queue.async {
      continuation.resume(returning: dc_add_chat(handle.raw,"c","Later unreviewed item",project.path) != 0)
    } }
    try require(addedLater, "unable to add late chat")
    store.reload()
    try await wait { store.chats.count == 3 }
    store.drafts = ["a":"draft A","c":"draft C"]
    store.selected = "a"
    let count = await withCheckedContinuation { continuation in
      store.deleteReviewedChats(reviewed) { continuation.resume(returning:$0) }
    }
    try require(count == 2 && store.chats.map(\.id) == ["c"], "delete scope changed")
    try require(store.drafts["a"] == nil && store.drafts["c"] == "draft C", "draft isolation")
    let kept = try String(contentsOf:file,encoding:.utf8)
    try require(kept == "project content", "project files changed")
    let fixture = Data("""
      [{"role":"user","content":"original #1","sequence":0,"sourceHash":"private-marker"},
       {"role":"assistant","content":"response","reasoning":"private-marker"},
       {"role":"tool","content":"private-marker"}]
      """.utf8)
    let messages = try JSONDecoder().decode([Message].self,from:fixture)
    for format in TranscriptFormat.allCases {
      let data = try format.contents(title:"聊天",messages:messages)
      let text = String(decoding:data,as:UTF8.self)
      try require(text.contains("original #1") && text.contains("response") && !text.contains("private-marker"), "transcript confidentiality or ordering")
      if format == .json {
        let parsed = try JSONSerialization.jsonObject(with:data) as? [String:Any]
        let rows = parsed?["messages"] as? [[String:String]]
        try require(rows?.map { $0["role"] } == ["user","assistant"], "invalid JSON format")
      }
    }
    print("PASS reviewed ordinary and archived chats removed, subsequent chat and project file retained, caches scoped correctly; Markdown and JSON exported user-visible text only")
  }
}
