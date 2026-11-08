import SwiftUI
import WebKit
import CryptoKit

struct PluginClient: Decodable,Identifiable {
  let source:String,package:String,version:String,runtimeID:String,digest:String,code:String
  let config:JSONValue
  var id:String {
    let encoder=JSONEncoder();encoder.outputFormatting = [.sortedKeys]
    return runtimeID+":"+digest+":"+SHA256.hash(data:(try? encoder.encode(config)) ?? Data()).map{String(format:"%02x",$0)}.joined()
  }
}
struct PluginClientDock: View {
  @EnvironmentObject var store:WorkspaceStore
  let chat:String
  var body:some View {
    ForEach(store.pluginClients) {client in
      PluginClientSurface(client:client,chat:chat) {
        store.extensionRequest(["op":"extensions.clientFault","source":client.source,"package":client.package,"version":client.version],as:[ExtensionRow].self) {_ in store.refreshPluginPages();store.pollExtensionHealth()}
      }.frame(height:72).id("\(store.identityEpoch):\(chat):\(client.id)")
    }
  }
}
// One ephemeral WebKit context per package/chat. The only native message is a
// bounded fault signal tied to this owner; there is no arbitrary tool bridge.
private struct PluginClientSurface:NSViewRepresentable {
  let client:PluginClient,chat:String,onFault:()->Void
  func makeCoordinator()->Coordinator {Coordinator(client:client,chat:chat,onFault:onFault)}
  func makeNSView(context:Context)->WKWebView {
    let config=WKWebViewConfiguration();config.websiteDataStore = .nonPersistent()
    config.setURLSchemeHandler(context.coordinator,forURLScheme:"dunlin-plugin")
    config.userContentController.add(context.coordinator,name:"surfaceFault")
    let view=WKWebView(frame:.zero,configuration:config);view.navigationDelegate=context.coordinator
    view.setValue(false,forKey:"drawsBackground")
    view.load(URLRequest(url:URL(string:"dunlin-plugin://surface/index.html")!));return view
  }
  func updateNSView(_ view:WKWebView,context:Context){}
  static func dismantleNSView(_ view:WKWebView,coordinator:Coordinator){
    coordinator.closed=true;view.stopLoading();view.evaluateJavaScript("globalThis.surface?.dispose()",completionHandler:nil)
    view.configuration.userContentController.removeScriptMessageHandler(forName:"surfaceFault");view.navigationDelegate=nil
  }
  final class Coordinator:NSObject,WKURLSchemeHandler,WKScriptMessageHandler,WKNavigationDelegate {
    var closed=false,failed=false
    let onFault:()->Void
    var assets:[String:(Data,String)]=[:]
    init(client:PluginClient,chat:String,onFault:@escaping()->Void){
      self.onFault=onFault;super.init()
      func literal<T:Encodable>(_ value:T)->String {String(data:(try? JSONEncoder().encode(value)) ?? Data("null".utf8),encoding:.utf8) ?? "null"}
      guard let root=Bundle.main.resourceURL,let runtime=try? Data(contentsOf:root.appendingPathComponent("ExtensionAssets/web/runtime.js")) else{return}
      let html="""
      <!doctype html><html><head><meta charset="utf-8"><meta http-equiv="Content-Security-Policy" content="default-src 'none'; script-src dunlin-plugin:; style-src 'unsafe-inline'; img-src data:; connect-src 'none'; frame-src 'none'; base-uri 'none'; form-action 'none'"><style>html,body{margin:0;background:transparent;color-scheme:light dark;font:13px -apple-system}#surface{padding:8px;overflow:auto}</style></head><body><div id="surface"></div><script src="runtime.js"></script><script src="client.js"></script></body></html>
      """
      let script="""
      (()=>{let registration;const fault=()=>window.webkit.messageHandlers.surfaceFault.postMessage('failed');
      window.addEventListener('error',fault);window.addEventListener('unhandledrejection',fault);
      globalThis.__ModuleLoader__={load(value){if(registration)throw Error('duplicate client');registration=value}};
      try{\(client.code)\n}catch(_){fault();return}
      (async()=>{globalThis.surface=await DunlinDSH.createSurface(\(literal(chat)),fault);await surface.load(registration,\(literal(client.package)),\(literal(client.config)));surface.mount(document.getElementById('surface'))})().catch(fault);
      })();
      """
      assets=["/index.html":(Data(html.utf8),"text/html"),"/runtime.js":(runtime,"text/javascript"),"/client.js":(Data(script.utf8),"text/javascript")]
    }
    func webView(_ webView:WKWebView,start task:any WKURLSchemeTask){
      guard !closed,let url=task.request.url,url.host=="surface",let asset=assets[url.path] else {task.didFailWithError(URLError(.resourceUnavailable));return}
      task.didReceive(URLResponse(url:url,mimeType:asset.1,expectedContentLength:asset.0.count,textEncodingName:"utf-8"));task.didReceive(asset.0);task.didFinish()
    }
    func webView(_ webView:WKWebView,stop task:any WKURLSchemeTask){}
    func userContentController(_ userContentController:WKUserContentController,didReceive message:WKScriptMessage){
      guard !closed,!failed,message.frameInfo.isMainFrame,message.body as? String == "failed" else{return};failed=true;onFault()
    }
    func webView(_ webView:WKWebView,decidePolicyFor action:WKNavigationAction,decisionHandler:@escaping(WKNavigationActionPolicy)->Void){decisionHandler(action.request.url?.absoluteString=="dunlin-plugin://surface/index.html" ? .allow:.cancel)}
    func webViewWebContentProcessDidTerminate(_ webView:WKWebView){if !closed && !failed {failed=true;onFault()}}
  }
}
