import SwiftUI
import WebKit
import CryptoKit

private func stablePluginJSON<T:Encodable>(_ value:T)->Data? {let encoder=JSONEncoder();encoder.outputFormatting=[.sortedKeys];return try? encoder.encode(value)}

struct PluginClient: Decodable,Identifiable {
  let source:String,package:String,version:String,runtimeID:String,digest:String,code:String
  let config:JSONValue
  let slots:[String]?
  let instance:String?
  let presentationPublisher:Bool?
  var owner:[String:Any] {var value:[String:Any] = ["source":source,"package":package,"version":version];if let instance {value["instance"]=instance};return value}
  var ownerID:String {runtimeID+":"+(instance ?? "")+":"+digest}
  var id:String {
    let encoder=JSONEncoder();encoder.outputFormatting = [.sortedKeys]
    return runtimeID+":"+(instance ?? "")+":"+digest+":"+SHA256.hash(data:(try? encoder.encode(config)) ?? Data()).map{String(format:"%02x",$0)}.joined()
  }
}
struct PluginClientDock: View {
  @EnvironmentObject var store:WorkspaceStore
  let chat:String
  var body:some View {
    ForEach(store.pluginClients.filter{($0.slots ?? ["conversation.composer.dock"]).contains("conversation.composer.dock")}) {client in
      PluginClientSurface(client:client,chat:chat,onFault:{ diagnostic in
        store.extensionRequest(client.owner.merging(["op":"extensions.clientFault","diagnostic":diagnostic]){_,new in new},as:[ExtensionRow].self) {_ in store.refreshPluginPages();store.pollExtensionHealth()}
      },onRequest:{command,args,reply in store.pluginClientRequest(client,chat:chat,command:command,args:args,reply:reply)}).frame(height:72).id("\(store.identityEpoch):\(chat):\(client.id)")
    }
  }
}
// One ephemeral WebKit context per package/chat. Requests use owner-bound,
// audited C extension commands; no arbitrary native selector is exposed.
struct PluginClientSurface:NSViewRepresentable {
  let client:PluginClient,chat:String,onFault:(String)->Void
  var storageIdentity=""
  var slot="conversation.composer.dock"
  var reasoningText: String? = nil
  var theme="system"
  var themeDefinition:JSONValue?=nil
  var presentations:JSONValue = .array([])
  var onTheme:(String)->Void = {_ in}
  var onContentHeight: ((CGFloat)->Void)? = nil
  var onRequest:(String,[String:Any],@escaping(Any?,String?)->Void)->Void = {_,_,reply in reply(nil,"Plugin transport unavailable")}
  static func storageID(_ identity:String,client:PluginClient)->UUID {
    let digest=Array(SHA256.hash(data:Data((identity+":"+client.source+":"+client.package).utf8)).prefix(16))
    return UUID(uuid:(digest[0],digest[1],digest[2],digest[3],digest[4],digest[5],digest[6],digest[7],digest[8],digest[9],digest[10],digest[11],digest[12],digest[13],digest[14],digest[15]))
  }
  func makeCoordinator()->Coordinator {Coordinator(client:client,chat:chat,onFault:onFault,slot:slot,reasoningText:reasoningText,theme:theme,themeDefinition:themeDefinition,presentations:presentations,onTheme:onTheme,onRequest:onRequest)}
  func makeNSView(context:Context)->WKWebView {
    let config=WKWebViewConfiguration();config.websiteDataStore = storageIdentity.isEmpty ? .nonPersistent() : WKWebsiteDataStore(forIdentifier:Self.storageID(storageIdentity,client:client))
    config.setURLSchemeHandler(context.coordinator,forURLScheme:"dunlin-plugin")
    config.userContentController.add(context.coordinator,name:"surfaceFault")
    config.userContentController.addScriptMessageHandler(context.coordinator,contentWorld:.page,name:"surfaceRPC")
    let view=WKWebView(frame:.zero,configuration:config);view.navigationDelegate=context.coordinator
    context.coordinator.onContentHeight = onContentHeight
    view.setValue(false,forKey:"drawsBackground")
    view.load(URLRequest(url:URL(string:"dunlin-plugin://surface/")!));return view
  }
  func updateNSView(_ view:WKWebView,context:Context) {
    let coordinator=context.coordinator
    if ["dunlin.workbench","conversation.chat.reasoning.body"].contains(slot),let configuration=stablePluginJSON(client.config),coordinator.configuration != configuration {
      coordinator.configuration=configuration
      coordinator.update("setConfiguration",arguments:String(decoding:configuration,as:UTF8.self),in:view)
    }
    let definition=stablePluginJSON(themeDefinition) ?? Data("null".utf8)
    let presentation=stablePluginJSON(presentations) ?? Data("[]".utf8)
    if coordinator.presentations != presentation {
      coordinator.presentations=presentation
      coordinator.update("setPresentations",arguments:String(decoding:presentation,as:UTF8.self),in:view)
    }
    guard coordinator.theme != theme || coordinator.themeDefinition != definition,
      let encodedTheme=try? JSONEncoder().encode(theme) else{return}
    coordinator.theme=theme
    coordinator.themeDefinition=definition
    coordinator.update("setThemeDefinition",arguments:String(decoding:definition,as:UTF8.self)+","+String(decoding:encodedTheme,as:UTF8.self),in:view)
  }
  static func dismantleNSView(_ view:WKWebView,coordinator:Coordinator){
    coordinator.onRequest("ui.close",["_surface":coordinator.surfaceID]){_,_ in};coordinator.closed=true;coordinator.downloads.removeAll();for (_,handle) in coordinator.resourceHandles{coordinator.onRequest(handle.0,["id":handle.1,"release":true]){_,_ in}};coordinator.resourceHandles.removeAll();view.stopLoading();view.evaluateJavaScript("globalThis.surface?.dispose?.()",completionHandler:nil)
    view.configuration.userContentController.removeScriptMessageHandler(forName:"surfaceFault");view.configuration.userContentController.removeScriptMessageHandler(forName:"surfaceRPC");view.navigationDelegate=nil
  }
  final class Coordinator:NSObject,WKURLSchemeHandler,WKScriptMessageHandler,WKScriptMessageHandlerWithReply,WKNavigationDelegate {
    let surfaceID=UUID().uuidString
    var onContentHeight: ((CGFloat)->Void)?
    var closed=false,failed=false
    private var surfaceReady=false
    private var pendingUpdates:[String:String]=[:]
    // WebKit exposes an element named "surface" on window before React mounts.
    // Keep only the newest host value per method until the runtime is ready.
    func update(_ method:String,arguments:String,in view:WKWebView) {
      guard !closed,!failed else {return}
      guard surfaceReady else {pendingUpdates[method]=arguments;return}
      view.evaluateJavaScript("globalThis.surface."+method+"("+arguments+")",completionHandler:nil)
    }
    private func mounted(in view:WKWebView) {
      guard !surfaceReady else {return}
      surfaceReady=true
      for method in ["setConfiguration","setThemeDefinition","setPresentations"] {
        if let arguments=pendingUpdates.removeValue(forKey:method) {update(method,arguments:arguments,in:view)}
      }
    }
    var theme:String
    var themeDefinition:Data
    var presentations:Data
    var configuration:Data
    let onFault:(String)->Void
    let onTheme:(String)->Void
    let onRequest:(String,[String:Any],@escaping(Any?,String?)->Void)->Void
    var assets:[String:(Data,String)]=[:]
    var staticAssets:[String:(URL,String)]=[:]
    func staticAsset(_ path:String)->(Data,String)? {
      guard let (url,hash)=staticAssets[path],let handle=try? FileHandle(forReadingFrom:url) else{return nil};defer{try? handle.close()}
      guard let data=try? handle.read(upToCount:16_777_217),data.count<=16_777_216,SHA256.hash(data:data).map({String(format:"%02x",$0)}).joined()==hash else{return nil}
      let mime=path.hasSuffix(".json") ? "application/json":path.hasSuffix(".woff2") ? "font/woff2":path.hasSuffix(".svg") ? "image/svg+xml":path.hasSuffix(".png") ? "image/png":"application/octet-stream";return (data,mime)
    }
    var tasks=Set<ObjectIdentifier>()
    var resourceHandles:[ObjectIdentifier:(String,String)]=[:]
    struct Download {let name:String,size:Int;var data=Data();var saving=false}
    var downloads:[String:Download]=[:]
    init(client:PluginClient,chat:String,onFault:@escaping(String)->Void,slot:String,reasoningText:String?,theme:String,themeDefinition:JSONValue?,presentations:JSONValue,onTheme:@escaping(String)->Void,onRequest:@escaping(String,[String:Any],@escaping(Any?,String?)->Void)->Void){
      let lease=surfaceID;self.configuration=stablePluginJSON(client.config) ?? Data("{}".utf8);self.theme=theme;self.presentations=stablePluginJSON(presentations) ?? Data("[]".utf8);self.themeDefinition=stablePluginJSON(themeDefinition) ?? Data("null".utf8);self.onFault=onFault;self.onTheme=onTheme;self.onRequest={command,args,reply in var owned=args;owned["_surface"]=lease;onRequest(command,owned,reply)};super.init()
      func literal<T:Encodable>(_ value:T)->String {String(data:(try? JSONEncoder().encode(value)) ?? Data("null".utf8),encoding:.utf8) ?? "null"}
      guard let root=Bundle.main.resourceURL,let runtime=try? Data(contentsOf:root.appendingPathComponent("ExtensionAssets/web/runtime.js")) else{return}
      // This verified Pixi bundle generates renderer functions for its shaders.
      let pixiPolicy = client.package == "dsh-pet-live2d" && client.digest == "b6279c348e6e15ac3f29cf3430c1d106b9236660b835d1fd28acba05daa7a67b" ? " 'unsafe-eval'" : ""
      let intrinsic = slot == "conversation.chat.reasoning.body"
      let sizingCSS = intrinsic ? "html,body{height:auto}#surface{min-height:0}" : ""
      let measure = intrinsic ? """
      const element=document.getElementById('surface');
      let scheduled=false,last=-1;
      new ResizeObserver(()=>{
        if(scheduled)return;
        scheduled=true;
        requestAnimationFrame(()=>{
          scheduled=false;
          const height=Math.min(420,Math.max(32,Math.ceil(element.getBoundingClientRect().height)));
          if(height!==last){last=height;window.webkit.messageHandlers.surfaceRPC.postMessage({contentHeight:height})}
        })
      }).observe(element);
      """ : ""
      let html="""
      <!doctype html><html><head><meta charset="utf-8"><meta http-equiv="Content-Security-Policy" content="default-src 'none'; script-src dunlin-plugin:\(pixiPolicy); style-src 'unsafe-inline' dunlin-plugin:; img-src data: blob: dunlin-plugin:; media-src blob: dunlin-plugin:; connect-src \(pixiPolicy.isEmpty ? "'none'" : "dunlin-plugin://surface"); font-src data: dunlin-plugin:; frame-src dunlin-plugin: blob: https: http:; worker-src blob:; base-uri 'none'; form-action 'none'"><link rel="stylesheet" href="runtime.css"><style>html,body{margin:0;background:transparent;color-scheme:light dark;font:13px -apple-system}#surface{padding:8px;overflow:auto;min-height:100vh;box-sizing:border-box}html,body{height:100%}button{font:inherit;cursor:pointer}\(sizingCSS)</style></head><body><div id="surface"></div><script src="runtime.js"></script><script src="client.js"></script></body></html>
      """
      let script="""
      (()=>{let registration;
      const fault=e=>window.webkit.messageHandlers.surfaceFault.postMessage(DunlinDSH.surfaceDiagnostic(e));
      window.addEventListener('error',fault);window.addEventListener('unhandledrejection',fault);
      const rpc=(command,args)=>window.webkit.messageHandlers.surfaceRPC.postMessage({command,args});
      DunlinDSH.installTransport(rpc);
      \(measure)
      globalThis.__ModuleLoader__={load(value){if(registration)throw Error('duplicate client');registration=value}};
      try{\(client.code)\n}catch(e){fault(e);return}
      const options={chat:\(literal(chat)),slot:\(literal(slot)),reasoningText:\(literal(reasoningText)),theme:\(literal(theme)),themeDefinition:\(literal(themeDefinition)),presentations:\(literal(presentations)),presentationPublisher:\(literal(client.presentationPublisher == true)),package:\(literal(client.package)),config:\(literal(client.config))};
      DunlinDSH.mountNativeSurface(DunlinDSH,registration,options,rpc,fault,theme=>window.webkit.messageHandlers.surfaceRPC.postMessage({theme})).then(()=>window.webkit.messageHandlers.surfaceRPC.postMessage({surfaceReady:true})).catch(fault);
      })();
      """
      let css=(try? Data(contentsOf:root.appendingPathComponent("ExtensionAssets/web/runtime.css"))) ?? Data()
      if let data=try? Data(contentsOf:root.appendingPathComponent("ExtensionAssets/assets.json")),let manifest=(try? JSONSerialization.jsonObject(with:data)) as? [String:Any],let files=manifest["files"] as? [[String:Any]] {
        for item in files {guard let path=item["path"] as? String,path.hasPrefix("web/tldraw/5.3.0/"),!path.split(separator:"/").contains(".."),let hash=item["sha256"] as? String,hash.count==64 else{continue};staticAssets["/dunlin-assets/"+String(path.dropFirst(4))]=(root.appendingPathComponent("ExtensionAssets/"+path),hash)}
      }
      assets=["/":(Data(html.utf8),"text/html"),"/runtime.css":(css,"text/css"),"/index.html":(Data(html.utf8),"text/html"),"/runtime.js":(runtime,"text/javascript"),"/client.js":(Data(script.utf8),"text/javascript")]
    }
    func webView(_ webView:WKWebView,start task:any WKURLSchemeTask){
      guard !closed,let url=task.request.url,url.host=="surface" else {task.didFailWithError(URLError(.resourceUnavailable));return}
      let token=ObjectIdentifier(task);tasks.insert(token)
      func finish(_ bytes:Data,_ mime:String,_ status:Int=200,_ headers:[String:String]=[:]){resourceHandles.removeValue(forKey:token);guard !closed,tasks.remove(token) != nil else{return};task.didReceive(HTTPURLResponse(url:url,statusCode:status,httpVersion:"HTTP/1.1",headerFields:headers.merging(["Content-Type":mime]){_,new in new})!);task.didReceive(bytes);task.didFinish()}
      if let asset=assets[url.path]{finish(asset.0,asset.1);return}
      if staticAssets[url.path] != nil {onRequest("storage.read",[:]){_,error in guard error==nil,let asset=self.staticAsset(url.path) else{if self.tasks.remove(token) != nil {task.didFailWithError(URLError(.resourceUnavailable))};return};finish(asset.0,asset.1)};return}
      func loadBody(_ response:[String:Any],_ handle:String,_ offset:Int,_ bytes:Data){
        guard !closed,tasks.contains(token) else{return}
        let total=response["bytes"] as? Int ?? 0
        guard total<=16_777_216 else{tasks.remove(token);task.didFailWithError(URLError(.dataLengthExceedsMaximum));return}
        if offset>=total {onRequest("http.body",["id":handle,"release":true]){_,_ in};finish(bytes,(response["headers"] as? [String:String])?["content-type"] ?? "application/octet-stream",response["status"] as? Int ?? 200,response["headers"] as? [String:String] ?? [:]);return}
        onRequest("http.body",["id":handle,"offset":offset]){value,error in
          guard error==nil,let part=value as? [String:Any],let raw=part["base64"] as? String,let data=Data(base64Encoded:raw),!data.isEmpty else{self.resourceHandles.removeValue(forKey:token);self.onRequest("http.body",["id":handle,"release":true]){_,_ in};if self.tasks.remove(token) != nil {task.didFailWithError(URLError(.cannotDecodeRawData))};return}
          var next=bytes;next.append(data);loadBody(response,handle,offset+data.count,next)
        }
      }
      func consume(_ value:Any?,_ error:String?){
        guard !self.closed,self.tasks.contains(token) else{if let response=value as? [String:Any],let handle=response["pendingHandle"] as? String{self.onRequest("http.pending",["id":handle,"release":true]){_,_ in}};return}
        guard error==nil,let response=value as? [String:Any] else{self.resourceHandles.removeValue(forKey:token);self.tasks.remove(token);task.didFailWithError(URLError(.cannotLoadFromNetwork));return}
        if let handle=response["pendingHandle"] as? String {
          self.resourceHandles[token]=("http.pending",handle)
          func poll(){
            guard !self.closed,self.tasks.contains(token) else{return}
            self.onRequest("http.pending",["id":handle]){value,error in
              guard error==nil,let row=value as? [String:Any] else{consume(nil,error ?? "Plugin request failed");return}
              if row["ready"] as? Bool == true {self.resourceHandles.removeValue(forKey:token);self.onRequest("http.pending",["id":handle,"release":true]){_,_ in};consume(row["response"],nil)}
              else {DispatchQueue.main.asyncAfter(deadline:.now()+0.2){poll()}}
            }
          }
          poll();return
        }
        if let handle=response["bodyHandle"] as? String {self.resourceHandles[token]=("http.body",handle);loadBody(response,handle,0,Data());return}
        let bytes=(response["bodyBase64"] as? String).flatMap{Data(base64Encoded:$0)} ?? (try? JSONSerialization.data(withJSONObject:response["body"] ?? NSNull(),options:.fragmentsAllowed)) ?? Data()
        finish(bytes,(response["headers"] as? [String:String])?["content-type"] ?? "application/octet-stream",response["status"] as? Int ?? 200,response["headers"] as? [String:String] ?? [:])
      }
      onRequest("http",["url":url.path(percentEncoded:true)+(url.query(percentEncoded:true).map{"?"+$0} ?? ""),"method":"GET"],consume)
    }
    func webView(_ webView:WKWebView,stop task:any WKURLSchemeTask){let token=ObjectIdentifier(task);tasks.remove(token);if let (method,id)=resourceHandles.removeValue(forKey:token){onRequest(method,["id":id,"release":true]){_,_ in}}}
    func userContentController(_ userContentController:WKUserContentController,didReceive message:WKScriptMessage){
      guard !closed,!failed,message.frameInfo.isMainFrame,let diagnostic=message.body as? String else{return};failed=true;onFault(String(diagnostic.prefix(128)))
    }
    func userContentController(_ userContentController:WKUserContentController,didReceive message:WKScriptMessage,replyHandler:@escaping(Any?,String?)->Void){
      guard !closed,message.frameInfo.isMainFrame,let body=message.body as? [String:Any] else{replyHandler(nil,"Plugin surface unavailable");return}
      if let height = body["contentHeight"] as? Int, let onContentHeight, (32...420).contains(height) {
        onContentHeight(CGFloat(height)); replyHandler(true,nil); return
      }
      if body["surfaceReady"] as? Bool == true,let view=message.webView {mounted(in:view);replyHandler(true,nil);return}
      if let theme=body["theme"] as? String {onTheme(theme);replyHandler(true,nil);return}
      if let command=body["command"] as? String,command.hasPrefix("download."),let args=body["args"] as? [String:Any]{download(command,args,replyHandler);return}
      guard let command=body["command"] as? String,(["http","http.pending","http.body","http.events","storage.read","storage.write","theme.presentation","rpc","reasoning.service","config.set","ui.close"].contains(command) || command.hasPrefix("native.") || command.hasPrefix("http.upload.") || command.hasPrefix("ws:")),command.count<300,let args=body["args"] as? [String:Any] else{replyHandler(nil,"Unsupported plugin request");return}
      if command=="native.asset.read" {guard let path=args["path"] as? String else{replyHandler(nil,"Invalid asset path");return};onRequest("storage.read",[:]){_,error in guard !self.closed,error==nil,let asset=self.staticAsset(path) else{replyHandler(nil,"Static asset unavailable");return};replyHandler(["base64":asset.0.base64EncodedString(),"type":asset.1],nil)};return}
      onRequest(command,args,replyHandler)
    }
    func download(_ command:String,_ args:[String:Any],_ reply:@escaping(Any?,String?)->Void){
      if command=="download.error" {let alert=NSAlert();alert.messageText="导出未完成";alert.informativeText=String((args["message"] as? String ?? "无法导出文件").prefix(300));if let window=NSApp.keyWindow{alert.beginSheetModal(for:window){_ in}};reply(false,nil);return}
      if command=="download.begin" {
        guard downloads.isEmpty,let size=args["bytes"] as? Int,(0...16_777_216).contains(size),let name=args["name"] as? String,!name.contains("\0") else{reply(nil,"Invalid or excessive download");return}
        let id=UUID().uuidString;let safe=String((name as NSString).lastPathComponent.prefix(160));downloads[id]=Download(name:safe.isEmpty || safe=="." || safe==".." ? "download":safe,size:size)
        onRequest("storage.read",[:]){_,error in guard !self.closed,error==nil else{self.downloads.removeValue(forKey:id);reply(nil,"Plugin owner unavailable");return};reply(["id":id],nil)};return
      }
      guard let id=args["id"] as? String,var row=downloads[id] else{reply(nil,"Download unavailable");return}
      if command=="download.cancel" {downloads.removeValue(forKey:id);reply(true,nil);return}
      if command=="download.append" {
        guard !row.saving,let offset=args["offset"] as? Int,offset==row.data.count,let raw=args["base64"] as? String,raw.utf8.count<=65_536,let bytes=Data(base64Encoded:raw),!bytes.isEmpty,bytes.count<=row.size-row.data.count else{reply(nil,"Invalid download chunk");return}
        row.data.append(bytes);downloads[id]=row;reply(true,nil);return
      }
      guard command=="download.finish",!row.saving,row.data.count==row.size else{reply(nil,"Incomplete download");return};row.saving=true;downloads[id]=row
      // The destination is chosen by the user. A second ownership check after
      // the panel closes prevents a disabled/replaced plugin from writing.
      let panel=NSSavePanel();panel.nameFieldStringValue=row.name;panel.canCreateDirectories=true
      panel.begin{result in
        guard !self.closed,self.downloads[id] != nil else{reply(nil,"Plugin surface closed");return}
        guard result == .OK,let url=panel.url else{self.downloads.removeValue(forKey:id);reply(["saved":false],nil);return}
        self.onRequest("storage.read",[:]){_,error in
          defer{self.downloads.removeValue(forKey:id)}
          guard !self.closed,error==nil else{reply(nil,"Plugin owner changed");return}
          do{try row.data.write(to:url,options:.atomic);reply(["saved":true],nil)}catch{reply(nil,"Unable to save the selected file")}
        }
      }
    }
    func webView(_ webView:WKWebView,decidePolicyFor action:WKNavigationAction,decisionHandler:@escaping(WKNavigationActionPolicy)->Void){
      guard !closed,let url=action.request.url else{decisionHandler(.cancel);return}
      if action.targetFrame?.isMainFrame == false {
        let own=url.scheme=="dunlin-plugin" && url.host=="surface"
        decisionHandler(own || url.scheme=="blob" || ["https","http"].contains(url.scheme ?? "") ? .allow:.cancel);return
      }
      if url.absoluteString=="dunlin-plugin://surface/" {decisionHandler(.allow);return}
      if action.navigationType == .linkActivated,["https","http"].contains(url.scheme ?? ""){NSWorkspace.shared.open(url)}
      decisionHandler(.cancel)
    }
    func webViewWebContentProcessDidTerminate(_ webView:WKWebView){if !closed && !failed {failed=true;onFault("WebKit content process terminated")}}
  }
}

struct PluginThemeSettings:View {
  @EnvironmentObject var store:WorkspaceStore
  var body:some View {
    ForEach(store.pluginClients.filter{($0.slots ?? []).contains("settings.general.item")}){client in
      PluginClientSurface(client:client,chat:"appearance",onFault:{ diagnostic in
        store.extensionRequest(client.owner.merging(["op":"extensions.clientFault","diagnostic":diagnostic]){_,new in new},as:[ExtensionRow].self){_ in store.refreshPluginPages();store.refreshThemes()}
      },slot:"settings.general.item",theme:store.installedThemes.first(where:{$0.id==store.theme})?.preference ?? store.theme,presentations:store.pluginPresentationValues,onTheme:{preference in
        if ["system","light","dark"].contains(preference){if store.theme != preference {store.theme=preference};return}
        if let selected=store.installedThemes.first(where:{$0.preference==preference && $0.id.hasPrefix("plugin:\(client.runtimeID):")}),store.theme != selected.id {store.theme=selected.id}
      },onRequest:{command,args,reply in
        store.pluginClientRequest(client,chat:nil,command:command,args:args,reply:reply)
      }).frame(height:650).id("\(store.identityEpoch):theme:\(client.id)")
    }
  }
}

extension WorkspaceStore {
  func pluginClientRequest(_ client:PluginClient,chat:String?,command:String,args:[String:Any],reply:@escaping(Any?,String?)->Void){
    if command=="theme.presentation" {
      var request=client.owner;request["op"]="extensions.presentation.write";request["value"]=args["value"]
      extensionRequest(request,as:Bool.self){r in reply(r.data,r.ok ? nil:r.error ?? "主题效果保存失败");if r.ok {self.refreshPresentations()}};return
    }
    if command.hasPrefix("native.") || command=="config.set" {pluginWorkbenchRequest(client,chat:chat,command:command,args:args,reply:reply);return}
    let storage=command.hasPrefix("storage.")
    var request:[String:Any]=["op":storage ? "extensions.clientStorage."+String(command.dropFirst(8)):"extensions.command","source":client.source,"package":client.package,"version":client.version]
    if let instance=client.instance {request["instance"]=instance}
    if storage {if let value=args["value"] {request["value"]=value}}
    else {request["command"]=command;request["arguments"]=args;if let chat {request["chat"]=chat}}
    extensionRequest(request,as:JSONValue.self){result in
      guard result.ok,let envelope=result.data else{reply(nil,extensionReason(result.error ?? "Plugin request failed"));self.pollExtensionHealth();return}
      let response:JSONValue
      if storage {response=envelope} else if case .object(let fields)=envelope,let value=fields["data"] {response=value} else{reply(nil,"Invalid plugin response");return}
      if client.package=="dsh-project-manager",command=="http",(args["url"] as? String)?.contains("/session/bind")==true {self.reload()}
      guard let bytes=try? JSONEncoder().encode(response),let object=try? JSONSerialization.jsonObject(with:bytes,options:.fragmentsAllowed) else{reply(nil,"Invalid plugin response");return}
      if client.package=="dshmarket",command=="http",args["method"] as? String == "POST",let path=args["url"] as? String,["/dsh-market/toggle","/dsh-market/install","/dsh-market/update","/dsh-market/uninstall"].contains(path),let value=object as? [String:Any],let status=value["status"] as? Int,(200..<300).contains(status){self.refreshPluginPages();self.refreshThemes();self.pollExtensionHealth()}
      reply(object,nil)
      if !storage {self.startNativePluginJobs()}
    }
  }
}
