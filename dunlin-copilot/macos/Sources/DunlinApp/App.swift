import AppKit
import CDesktop
import Security
import SwiftUI

@MainActor final class AppDelegate: NSObject, NSApplicationDelegate {
  static weak var store: WorkspaceStore?
  func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
    guard let store = Self.store else { return .terminateNow }
    if !store.busy {
      store.draftSave?.cancel();if let id=store.selected {store.persistDraft(id)};store.saveSettings()
      store.queue.async { DispatchQueue.main.async {sender.reply(toApplicationShouldTerminate:true)} }
      return .terminateLater
    }
    store.stop()
    let alert = NSAlert()
    alert.messageText = "正在停止当前任务"
    alert.informativeText = "任务结算后可再次退出。"
    alert.runModal()
    return .terminateCancel
  }
}
@main struct DunlinApp: App {
  @NSApplicationDelegateAdaptor(AppDelegate.self) var appDelegate
  @StateObject private var store = WorkspaceStore()
  var body: some Scene {
    WindowGroup {
      MainView().environmentObject(store).onAppear {
        AppDelegate.store = store
        if ProcessInfo.processInfo.arguments.contains("--smoke-test") {
          DispatchQueue.main.asyncAfter(deadline: .now() + 1) { store.newChat() }
          DispatchQueue.main.asyncAfter(deadline: .now() + 3) {
            guard let window = NSApp.windows.first(where: { $0.isVisible }),
              let view = window.contentView,
              let rep = view.bitmapImageRepForCachingDisplay(in: view.bounds)
            else { exit(2) }
            view.cacheDisplay(in: view.bounds, to: rep)
            do {
              guard let data = rep.representation(using: .png, properties: [:]) else { exit(3) }
              try data.write(to: store.directory.appendingPathComponent("window.png"))
              print(
                "PASS native window and chat lifecycle; partial bitmap; chats=\(store.chats.count)")
              exit(store.chats.isEmpty ? 4 : 0)
            } catch { exit(5) }
          }
        }
      }
    }
    .defaultSize(width: 1360, height: 830)
    Window("通话", id: "media-call") { MediaCallView().environmentObject(store) }.defaultSize(width:720,height:760)
    Settings { SettingsView().environmentObject(store) }.defaultSize(width:1360,height:830)
  }

}
