import Combine
import AppKit
import SwiftUI

private let companionTitles = [
  "dsh-pet-live2d": "Live2D 看板娘",
  "dsh-pet": "蓝色大肥鱼",
  "dsh-pet-sayaka": "美树沙耶香",
  "dsh-pet-dafeiyu": "Fairy 桌宠"
]

struct ChatCompanions: View {
  @EnvironmentObject var store: WorkspaceStore
  let chat: String
  @State private var selection: String?
  @State private var showing = false

  var body: some View {
    let clients = store.pluginClients.filter { ($0.slots?.contains("shell.overlay") ?? false) || companionTitles[$0.package] != nil }
    if let first = clients.first {
      Button { showing.toggle() } label: { Label("桌宠", systemImage: "pawprint") }
        .accessibilityIdentifier("chat.companions")
        .popover(isPresented: $showing) {
          let client = clients.first { $0.ownerID == selection } ?? first
          VStack(spacing: 0) {
            HStack {
              Picker("桌宠", selection: Binding(get: { client.ownerID }, set: { selection = $0 })) {
                ForEach(clients) { pet in Text(pluginWorkbenchTitle(pet.package)).tag(pet.ownerID) }
              }
              Spacer()
              Button("桌面悬浮") {
                showing = false
                store.companionPanel.show(store: store, client: client, chat: chat)
              }.accessibilityIdentifier("companion.float")
              Button("完成") { showing = false }
            }.padding(12)
            Divider()
            CompanionSurface(client: client, chat: chat).id(client.ownerID + chat)
          }.frame(width: 440, height: 380)
        }
    }
  }
}

private struct CompanionSurface: View {
  @EnvironmentObject var store: WorkspaceStore
  let client: PluginClient
  let chat: String
  var body: some View {
    PluginClientSurface(client: client, chat: chat, onFault: { diagnostic in
              store.extensionRequest(client.owner.merging(["op":"extensions.clientFault","diagnostic":diagnostic]) { _, new in new }, as: [ExtensionRow].self) { _ in
                store.refreshPluginPages()
                store.pollExtensionHealth()
              }
            }, storageIdentity: store.directory.path, slot: "shell.overlay", onRequest: { command, args, reply in
              store.pluginClientRequest(client, chat: chat, command: command, args: args, reply: reply)
            })
  }
}

// An overlay plug-in receives pointer coordinates relative to its viewport.
// Hand dragging over to the system so the same gesture continues across
// displays without requiring plug-in-specific geometry.
@MainActor private final class CompanionWindow: NSPanel {
  private var press: NSEvent?
  private var releaseObserver: Timer?
  private var awaitingNextPress = false

  private func finishDrag(_ event: NSEvent) {
    releaseObserver?.invalidate()
    releaseObserver = nil
    awaitingNextPress = true
    let pointer = NSEvent.mouseLocation
    let localRelease = convertPoint(fromScreen: pointer)
    // Leave a reachable grab point at the exposed outer edges. At a shared
    // edge the same position is reachable from the adjoining screen.
    if let screen = NSScreen.screens.first(where: { $0.frame.contains(pointer) }) {
      let f = screen.frame, safe = screen.visibleFrame.insetBy(dx: 48, dy: 48)
      func exposed(_ x: CGFloat, _ y: CGFloat) -> Bool {
        !NSScreen.screens.contains { $0 !== screen && $0.frame.contains(NSPoint(x: x, y: y)) }
      }
      var offset = NSPoint.zero
      if pointer.x < safe.minX && exposed(f.minX-1, pointer.y) { offset.x = max(safe.minX-pointer.x, screen.visibleFrame.minX-frame.minX) }
      if pointer.x > safe.maxX && exposed(f.maxX+1, pointer.y) { offset.x = min(safe.maxX-pointer.x, screen.visibleFrame.maxX-frame.maxX) }
      if pointer.y < safe.minY && exposed(pointer.x, f.minY-1) { offset.y = max(safe.minY-pointer.y, screen.visibleFrame.minY-frame.minY) }
      if pointer.y > safe.maxY && exposed(pointer.x, f.maxY+1) { offset.y = min(safe.maxY-pointer.y, screen.visibleFrame.maxY-frame.maxY) }
      if offset != .zero { setFrameOrigin(NSPoint(x: frame.minX+offset.x, y: frame.minY+offset.y)) }
    }
    if let up = NSEvent.mouseEvent(with: .leftMouseUp, location: localRelease,
                                   modifierFlags: event.modifierFlags, timestamp: ProcessInfo.processInfo.systemUptime,
                                   windowNumber: windowNumber, context: nil, eventNumber: event.eventNumber,
                                   clickCount: event.clickCount, pressure: 0) {
      super.sendEvent(up)
    }
  }

  override func sendEvent(_ event: NSEvent) {
    switch event.type {
    case .leftMouseDown:
      awaitingNextPress = false
      press = contentLayoutRect.insetBy(dx: 6, dy: 6).contains(event.locationInWindow) ? event : nil
    case .leftMouseDragged:
      if releaseObserver != nil { return }
      guard let down = press else { break }
      let start = down.locationInWindow, point = event.locationInWindow
      guard hypot(point.x-start.x, point.y-start.y) >= 5 else { break }
      press = nil
      super.sendEvent(event)
      // WindowServer expects the original down. Its drag session is asynchronous
      // and may consume the up; settle WebKit's capture exactly once.
      let timer = Timer(timeInterval: 1.0/60, repeats: true) { [weak self] timer in
        MainActor.assumeIsolated {
          guard let self else { timer.invalidate(); return }
          if NSEvent.pressedMouseButtons & 1 == 0 { self.finishDrag(down) }
        }
      }
      releaseObserver = timer
      RunLoop.main.add(timer, forMode: .common)
      performDrag(with: down)
      return
    case .leftMouseUp:
      if releaseObserver != nil { finishDrag(event); return }
      if awaitingNextPress { return }
      press = nil
    default: break
    }
    super.sendEvent(event)
  }

  override func close() {
    releaseObserver?.invalidate()
    releaseObserver = nil
    press = nil
    super.close()
  }

}

@MainActor final class CompanionPanel: NSObject, NSWindowDelegate {
  private var window: NSPanel?
  private var ownerChanges: AnyCancellable?

  func show(store: WorkspaceStore, client: PluginClient, chat: String) {
    window?.close()
    let screen = NSApp.keyWindow?.screen ?? NSScreen.main
    let available = screen?.visibleFrame.size ?? NSSize(width: 1000, height: 760)
    let size = NSSize(width: min(available.width, max(440, available.width * 0.48)),
                      height: min(available.height, max(350, available.height * 0.60)))
    let panel = CompanionWindow(contentRect: NSRect(origin: .zero, size: size),
                        styleMask: [.titled, .closable, .utilityWindow, .resizable],
                        backing: .buffered, defer: false)
    panel.title = pluginWorkbenchTitle(client.package)
    panel.titlebarAppearsTransparent = true
    panel.backgroundColor = .clear
    panel.isOpaque = false
    panel.hasShadow = false
    panel.isReleasedWhenClosed = false
    panel.isFloatingPanel = true
    panel.level = .floating
    panel.hidesOnDeactivate = false
    panel.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary]
    panel.contentView = NSHostingView(rootView: CompanionSurface(client: client, chat: chat).environmentObject(store))
    panel.delegate = self
    window = panel
    let epoch = store.identityEpoch
    ownerChanges = store.$pluginClients.sink { [weak store, weak panel] clients in
      if store?.identityEpoch != epoch || !clients.contains(where: { $0.ownerID == client.ownerID }) { panel?.close() }
    }
    guard window === panel else { return }
    panel.center()
    panel.makeKeyAndOrderFront(nil)
  }

  func windowWillClose(_ notification: Notification) {
    guard let panel = notification.object as? NSPanel, window === panel else { return }
    ownerChanges?.cancel()
    ownerChanges = nil
    panel.contentView = nil
    window = nil
  }
}

func pluginWorkbenchTitle(_ package:String)->String {
  if let title = companionTitles[package] { return title }
  switch package {case "dsh-better-sidebar":return "文件与侧栏";case "dsh-project-manager":return "项目看板";case "dsh-image-gen":return "图像工作台";case "dshmarket":return "DSH 市场";case "dsh-checkpoint-rewind":return "检查点";default:return package}
}
struct PluginWorkbenchView:View {
  @EnvironmentObject var store:WorkspaceStore
  @Environment(\.colorScheme) private var colorScheme
  let client:PluginClient
  @State private var commands:[NativeContribution]=[]
  @State private var commandsOwner=""
  @State private var selectedCommand:ExtensionCommandSelection?
  var body:some View {
    let chat=store.selected ?? store.chats.first(where:{!$0.archived})?.id
    VStack(alignment:.leading,spacing:0){
      HStack {Text(pluginWorkbenchTitle(client.package)).font(.title2.bold());Spacer();if !commands.isEmpty {Menu("命令") {ForEach(commands){item in Button(item.title ?? item.command ?? "命令"){selectedCommand=ExtensionCommandSelection(source:client.source,package:client.package,command:item.command ?? "",title:item.title ?? client.package,version:client.version,instance:client.instance)}}}.fixedSize().accessibilityIdentifier("plugin.commands")};Text(client.version).font(.caption).foregroundStyle(.secondary)}.padding(.horizontal,24).padding(.vertical,14)
      Divider()
      PluginClientSurface(client:client,chat:chat ?? "workbench",onFault:{ diagnostic in
        store.extensionRequest(client.owner.merging(["op":"extensions.clientFault","diagnostic":diagnostic]){_,new in new},as:[ExtensionRow].self){_ in store.refreshPluginPages();store.pollExtensionHealth()}
      },storageIdentity:store.directory.path,slot:"dunlin.workbench",theme:(store.themeScheme ?? colorScheme) == .dark ? "dark":"light",themeDefinition:store.pluginThemeDefinition,presentations:store.pluginPresentationValues,onRequest:{command,args,reply in
        store.pluginClientRequest(client,chat:chat,command:command,args:args,reply:reply)
      }).id("\(store.identityEpoch):workbench:\(chat ?? "none"):\(client.ownerID)")
    }.task(id:client.ownerID) {
      let owner=client.ownerID;commandsOwner=owner;commands=[];selectedCommand=nil
      store.extensionRequest(["op":"extensions.contributions"],as:[NativeContribution].self){r in
        guard commandsOwner==owner else{return};commands=(r.data ?? []).filter{$0.plugin==client.runtimeID && $0.kind=="command" && ($0.command?.hasPrefix("slash:") ?? false)}
      }
    }
      .sheet(item:$selectedCommand,onDismiss:{store.reload();store.reloadProjects()}) {selection in ExtensionCommandSheet(selection:selection)}
  }
}
extension WorkspaceStore {
  func pluginWorkbenchRequest(_ client:PluginClient,chat:String?,command:String,args:[String:Any],reply:@escaping(Any?,String?)->Void){
    let owner=client.owner
    func request(_ op:String,_ extra:[String:Any]=[:],done:@escaping(WorkspaceReply<JSONValue>)->Void){var body=owner;body["op"]=op;if let chat {body["chat"]=chat};body.merge(extra){_,new in new};extensionRequest(body,as:JSONValue.self,completion:done)}
    func object(_ value:JSONValue?)->Any? {guard let value,let bytes=try? JSONEncoder().encode(value) else{return nil};return try? JSONSerialization.jsonObject(with:bytes,options:.fragmentsAllowed)}
    func finish(_ result:WorkspaceReply<JSONValue>){guard result.ok else {reply(nil,result.error ?? "插件操作失败");return};reply(object(result.data),nil)}
    if command=="native.snapshot" {request("extensions.clientContext",done:finish);return}
    if command=="config.set" {
      guard let field=args["field"] as? String,!field.isEmpty,field.count<=128,!["__proto__","constructor","prototype"].contains(field),let value=args["value"] else{reply(nil,"无效的设置字段");return}
      request("extensions.config.get"){loaded in
        guard loaded.ok,let data=object(loaded.data) as? [String:Any],var config=data["config"] as? [String:Any],let hash=data["hash"] as? String else{reply(nil,"配置读取失败");return}
        config[field]=value
        request("extensions.settings.apply",["config":config,"expectedHash":hash]){saved in
          guard saved.ok else{reply(nil,saved.error ?? "配置保存失败");return};if (object(saved.data) as? [String:Any])?["rebound"] as? Bool == true {self.refreshPluginPages()};reply(config,nil)
        }
      };return
    }
    // UI-only actions also validate the current active owner/version in C.
    request("extensions.clientContext"){validated in
      guard validated.ok,let snapshot=object(validated.data) as? [String:Any] else{reply(nil,validated.error ?? "插件已停用");return}
      if command=="native.clipboard.write" {
        guard let text=args["text"] as? String,text.utf8.count<=65_536,!text.contains("\0") else{reply(nil,"复制内容超过限制或格式无效");return}
        NSPasteboard.general.clearContents();guard NSPasteboard.general.setString(text,forType:.string) else{reply(nil,"无法写入剪贴板");return};reply(["copied":true],nil);return
      }
      if command=="native.directory.pick" {let panel=NSOpenPanel();panel.canChooseFiles=false;panel.canChooseDirectories=true;panel.allowsMultipleSelection=false;reply(panel.runModal() == .OK ? panel.url?.path:NSNull(),nil);return}
      if command=="native.session.open" {
        guard let id=args["id"] as? String,let sessions=snapshot["sessions"] as? [String:Any],let byId=sessions["byId"] as? [String:Any],byId[id] != nil else{reply(nil,"会话不存在");return}
        if let selected=self.selected {self.persistDraft(selected)};self.selected=id;self.route="chat";reply(["value":true,"snapshot":snapshot],nil);return
      }
      if command=="native.plugin.open" {let name=args["name"] as? String ?? "";if let page=self.pluginPages.first(where:{$0.package==name}){self.pluginPageID=page.id;self.route="plugin"};reply(["value":true,"snapshot":snapshot],nil);return}
      var method=String(command.dropFirst("native.".count)),arguments=args
      if command=="native.remoteSession" {
        let remoteMethod=args["method"] as? String ?? "",values=args["args"] as? [Any] ?? []
        if remoteMethod=="canOpenWorkspacePath" {reply(["ok":true,"value":true],nil);return}
        if remoteMethod=="list" {reply(["ok":true,"value":snapshot["sessions"] ?? [:]],nil);return}
        if remoteMethod=="openWorkspacePath",let input=values.first as? [String:Any],let path=input["path"] as? String {
          let url=URL(fileURLWithPath:path);if input["action"] as? String == "reveal" {NSWorkspace.shared.activateFileViewerSelecting([url]);reply(["ok":true,"value":true],nil)}else{reply(["ok":NSWorkspace.shared.open(url)],nil)};return
        }
        method="session."+remoteMethod;arguments=values.first as? [String:Any] ?? [:]
      }
      if method=="workspace.create",arguments["cwd"]==nil,arguments["path"]==nil {let panel=NSOpenPanel();panel.canChooseFiles=false;panel.canChooseDirectories=true;if panel.runModal() != .OK {reply(nil,"已取消选择文件夹");return};arguments["cwd"]=panel.url?.path}
      request("extensions.clientAction",["method":method,"arguments":arguments]){result in
        if result.ok {self.reload();self.reloadProjects();if method=="session.prompt" {self.startNativePluginJobs()}};finish(result)
      }
    }
  }
}
