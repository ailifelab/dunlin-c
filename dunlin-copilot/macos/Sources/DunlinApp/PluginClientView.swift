import SwiftUI
import WebKit
import CryptoKit

struct PluginClient: Decodable,Identifiable {
  let source:String,package:String,version:String,runtimeID:String,digest:String,code:String
  let config:JSONValue
  let slots:[String]?
  var id:String {
    let encoder=JSONEncoder();encoder.outputFormatting = [.sortedKeys]
    return runtimeID+":"+digest+":"+SHA256.hash(data:(try? encoder.encode(config)) ?? Data()).map{String(format:"%02x",$0)}.joined()
  }
}
struct PluginClientDock: View {
  @EnvironmentObject var store:WorkspaceStore
  let chat:String
  var body:some View {
    ForEach(store.pluginClients.filter{($0.slots ?? ["conversation.composer.dock"]).contains("conversation.composer.dock")}) {client in
      PluginClientSurface(client:client,chat:chat,onFault:{
        store.extensionRequest(["op":"extensions.clientFault","source":client.source,"package":client.package,"version":client.version],as:[ExtensionRow].self) {_ in store.refreshPluginPages();store.pollExtensionHealth()}
      },onRequest:{command,args,reply in store.pluginClientRequest(client,chat:chat,command:command,args:args,reply:reply)}).frame(height:72).id("\(store.identityEpoch):\(chat):\(client.id)")
    }
  }
}
// One ephemeral WebKit context per package/chat. Requests use owner-bound,
// audited C extension commands; no arbitrary native selector is exposed.
struct PluginClientSurface:NSViewRepresentable {
  let client:PluginClient,chat:String,onFault:()->Void
  var slot="conversation.composer.dock"
  var theme="system"
  var onTheme:(String)->Void = {_ in}
  var onRequest:(String,[String:Any],@escaping(Any?,String?)->Void)->Void = {_,_,reply in reply(nil,"Plugin transport unavailable")}
  func makeCoordinator()->Coordinator {Coordinator(client:client,chat:chat,onFault:onFault,slot:slot,theme:theme,onTheme:onTheme,onRequest:onRequest)}
  func makeNSView(context:Context)->WKWebView {
    let config=WKWebViewConfiguration();config.websiteDataStore = .nonPersistent()
    config.setURLSchemeHandler(context.coordinator,forURLScheme:"dunlin-plugin")
    config.userContentController.add(context.coordinator,name:"surfaceFault")
    config.userContentController.addScriptMessageHandler(context.coordinator,contentWorld:.page,name:"surfaceRPC")
    let view=WKWebView(frame:.zero,configuration:config);view.navigationDelegate=context.coordinator
    view.setValue(false,forKey:"drawsBackground")
    view.load(URLRequest(url:URL(string:"dunlin-plugin://surface/index.html")!));return view
  }
  func updateNSView(_ view:WKWebView,context:Context){
    guard context.coordinator.theme != theme else{return};context.coordinator.theme=theme
    if let data=try? JSONEncoder().encode(theme),let value=String(data:data,encoding:.utf8){view.evaluateJavaScript("globalThis.surface?.setTheme("+value+")",completionHandler:nil)}
  }
  static func dismantleNSView(_ view:WKWebView,coordinator:Coordinator){
    coordinator.closed=true;view.stopLoading();view.evaluateJavaScript("globalThis.surface?.dispose()",completionHandler:nil)
    view.configuration.userContentController.removeScriptMessageHandler(forName:"surfaceFault");view.configuration.userContentController.removeScriptMessageHandler(forName:"surfaceRPC");view.navigationDelegate=nil
  }
  final class Coordinator:NSObject,WKURLSchemeHandler,WKScriptMessageHandler,WKScriptMessageHandlerWithReply,WKNavigationDelegate {
    var closed=false,failed=false
    var theme:String
    let onFault:()->Void
    let onTheme:(String)->Void
    let onRequest:(String,[String:Any],@escaping(Any?,String?)->Void)->Void
    var assets:[String:(Data,String)]=[:]
    init(client:PluginClient,chat:String,onFault:@escaping()->Void,slot:String,theme:String,onTheme:@escaping(String)->Void,onRequest:@escaping(String,[String:Any],@escaping(Any?,String?)->Void)->Void){
      self.theme=theme;self.onFault=onFault;self.onTheme=onTheme;self.onRequest=onRequest;super.init()
      func literal<T:Encodable>(_ value:T)->String {String(data:(try? JSONEncoder().encode(value)) ?? Data("null".utf8),encoding:.utf8) ?? "null"}
      guard let root=Bundle.main.resourceURL,let runtime=try? Data(contentsOf:root.appendingPathComponent("ExtensionAssets/web/runtime.js")) else{return}
      let html="""
      <!doctype html><html><head><meta charset="utf-8"><meta http-equiv="Content-Security-Policy" content="default-src 'none'; script-src dunlin-plugin:; style-src 'unsafe-inline' dunlin-plugin:; img-src data:; connect-src 'none'; font-src data:; frame-src 'none'; base-uri 'none'; form-action 'none'"><link rel="stylesheet" href="runtime.css"><style>html,body{margin:0;background:transparent;color-scheme:light dark;font:13px -apple-system}#surface{padding:8px;overflow:auto}</style></head><body><div id="surface"></div><script src="runtime.js"></script><script src="client.js"></script></body></html>
      """
      let script="""
      (()=>{let registration;const fault=()=>window.webkit.messageHandlers.surfaceFault.postMessage('failed');
      window.addEventListener('error',fault);window.addEventListener('unhandledrejection',fault);
      const nativeRPC=(command,args)=>window.webkit.messageHandlers.surfaceRPC.postMessage({command,args});
      globalThis.fetch=async(input,init={})=>{const url=new URL(typeof input==='string'?input:input.url,location.href);if(url.protocol!==location.protocol||url.host!==location.host||url.username||url.password)throw Error('External plugin fetch refused');let body={};if(init.body){try{body=JSON.parse(init.body)}catch{throw Error('Unsupported request body')}}const response=await nativeRPC('http',{method:init.method??'GET',url:url.pathname+url.search,body});let bytes;if(response.bodyHandle){const parts=[];try{for(let offset=0;offset<response.bytes;offset+=98304){const part=await nativeRPC('http.body',{id:response.bodyHandle,offset});parts.push(Uint8Array.from(atob(part.base64),x=>x.charCodeAt(0)))}bytes=new Blob(parts)}finally{await nativeRPC('http.body',{id:response.bodyHandle,release:true})}}else{bytes=response.bodyBase64?Uint8Array.from(atob(response.bodyBase64),x=>x.charCodeAt(0)):JSON.stringify(response.body)}return new Response([204,205,304].includes(response.status)?null:bytes,{status:response.status,headers:response.headers})};
      globalThis.__ModuleLoader__={load(value){if(registration)throw Error('duplicate client');registration=value}};
      try{\(client.code)\n}catch(_){fault();return}
      (async()=>{const initial=await nativeRPC('storage.read',{});for(const [key,value]of Object.entries(initial))localStorage.setItem(key,value);const proto=Storage.prototype,set=proto.setItem,remove=proto.removeItem,clear=proto.clear;const persist=()=>{const value={};for(let i=0;i<localStorage.length;i++){const key=localStorage.key(i);value[key]=localStorage.getItem(key)}nativeRPC('storage.write',{value}).catch(fault)};proto.setItem=function(key,value){set.call(this,key,value);persist()};proto.removeItem=function(key){remove.call(this,key);persist()};proto.clear=function(){clear.call(this);persist()};globalThis.surface=await DunlinDSH.createSurface(\(literal(chat)),fault,{slot:\(literal(slot)),theme:\(literal(theme)),onTheme:value=>window.webkit.messageHandlers.surfaceRPC.postMessage({theme:value})});await surface.load(registration,\(literal(client.package)),\(literal(client.config)));surface.mount(document.getElementById('surface'))})().catch(fault);
      })();
      """
      let css=(try? Data(contentsOf:root.appendingPathComponent("ExtensionAssets/web/runtime.css"))) ?? Data()
      assets=["/runtime.css":(css,"text/css"),"/index.html":(Data(html.utf8),"text/html"),"/runtime.js":(runtime,"text/javascript"),"/client.js":(Data(script.utf8),"text/javascript")]
    }
    func webView(_ webView:WKWebView,start task:any WKURLSchemeTask){
      guard !closed,let url=task.request.url,url.host=="surface",let asset=assets[url.path] else {task.didFailWithError(URLError(.resourceUnavailable));return}
      task.didReceive(URLResponse(url:url,mimeType:asset.1,expectedContentLength:asset.0.count,textEncodingName:"utf-8"));task.didReceive(asset.0);task.didFinish()
    }
    func webView(_ webView:WKWebView,stop task:any WKURLSchemeTask){}
    func userContentController(_ userContentController:WKUserContentController,didReceive message:WKScriptMessage){
      guard !closed,!failed,message.frameInfo.isMainFrame,message.body as? String == "failed" else{return};failed=true;onFault()
    }
    func userContentController(_ userContentController:WKUserContentController,didReceive message:WKScriptMessage,replyHandler:@escaping(Any?,String?)->Void){
      guard !closed,message.frameInfo.isMainFrame,let body=message.body as? [String:Any] else{replyHandler(nil,"Plugin surface unavailable");return}
      if let theme=body["theme"] as? String {onTheme(theme);replyHandler(true,nil);return}
      guard let command=body["command"] as? String,(["http","http.body","storage.read","storage.write"].contains(command) || command.hasPrefix("ws:")),command.count<300,let args=body["args"] as? [String:Any] else{replyHandler(nil,"Unsupported plugin request");return}
      onRequest(command,args,replyHandler)
    }
    func webView(_ webView:WKWebView,decidePolicyFor action:WKNavigationAction,decisionHandler:@escaping(WKNavigationActionPolicy)->Void){decisionHandler(action.request.url?.absoluteString=="dunlin-plugin://surface/index.html" ? .allow:.cancel)}
    func webViewWebContentProcessDidTerminate(_ webView:WKWebView){if !closed && !failed {failed=true;onFault()}}
  }
}

struct PluginThemeSettings:View {
  @EnvironmentObject var store:WorkspaceStore
  var body:some View {
    ForEach(store.pluginClients.filter{($0.slots ?? []).contains("settings.general.item")}){client in
      PluginClientSurface(client:client,chat:"appearance",onFault:{
        store.extensionRequest(["op":"extensions.clientFault","source":client.source,"package":client.package,"version":client.version],as:[ExtensionRow].self){_ in store.refreshPluginPages();store.refreshThemes()}
      },slot:"settings.general.item",theme:store.installedThemes.first(where:{$0.id==store.theme})?.preference ?? store.theme,onTheme:{preference in
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
    let storage=command.hasPrefix("storage.")
    var request:[String:Any]=["op":storage ? "extensions.clientStorage."+String(command.dropFirst(8)):"extensions.command","source":client.source,"package":client.package,"version":client.version]
    if storage {if let value=args["value"] {request["value"]=value}}
    else {request["command"]=command;request["arguments"]=args;if let chat {request["chat"]=chat}}
    extensionRequest(request,as:JSONValue.self){result in
      guard result.ok,let envelope=result.data else{reply(nil,result.error ?? "Plugin request failed");self.pollExtensionHealth();return}
      let response:JSONValue
      if storage {response=envelope} else if case .object(let fields)=envelope,let value=fields["data"] {response=value} else{reply(nil,"Invalid plugin response");return}
      guard let bytes=try? JSONEncoder().encode(response),let object=try? JSONSerialization.jsonObject(with:bytes,options:.fragmentsAllowed) else{reply(nil,"Invalid plugin response");return};reply(object,nil)
    }
  }
}
