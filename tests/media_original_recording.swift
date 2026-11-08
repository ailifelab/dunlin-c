import Foundation
import CDesktop
final class NativeStoreReference:@unchecked Sendable {
  let raw:OpaquePointer
  init(raw:OpaquePointer){self.raw=raw}
  deinit{dc_close(raw)}
}
@main struct RecordingIntegration {
  static func main() throws {
    let root=FileManager.default.temporaryDirectory.appendingPathComponent("dunlin-native-recording-"+UUID().uuidString)
    try FileManager.default.createDirectory(at:root,withIntermediateDirectories:false,attributes:[.posixPermissions:0o700])
    defer{try? FileManager.default.removeItem(at:root)}
    let store=NativeStoreReference(raw:dc_open(root.path)!),owner=DispatchQueue(label:"owned-recording-store")
    func req(_ value:[String:Any])->[String:Any]{owner.sync{
      let text=String(data:try! JSONSerialization.data(withJSONObject:value),encoding:.utf8)!
      let raw=dc_workspace_request(store.raw,text)!;defer{dc_free(raw)}
      let result=try! JSONSerialization.jsonObject(with:Data(String(cString:raw).utf8)) as! [String:Any];precondition(result["ok"] as? Bool==true);return result["data"] as? [String:Any] ?? [:]
    }}
    precondition(owner.sync{dc_add_chat(store.raw,"chat","Owned fixture","") != 0})
    for (call,overflow) in [("normal",false),("overflow",true)] {
      _=req(["op":"media.start","id":call,"chat":"chat","mode":"video","model":"fixture","saveText":false,"saveOriginal":true])
      let recorder=MediaOriginalRecording(store:store,owner:owner,call:call)!
      let warning=DispatchSemaphore(value:0);recorder.warning={warning.signal()}
      recorder.append(1,Data(repeating:0,count:4800));recorder.append(2,Data(repeating:0,count:4800))
      if overflow {recorder.append(3,Data(count:2_097_153))}
      recorder.finish();recorder.finish();recorder.append(1,Data(count:4800))
      let info=req(["op":"media.recording","id":call]);precondition(info["state"] as? String == (overflow ? "partial":"ready"))
      if overflow {precondition(warning.wait(timeout:.now()) == .success)}
      _=req(["op":"media.state","id":call,"state":"ended"])
    }
    print("PASS native asynchronous recording adapter drains packets into real C storage; bounded queue overflow persists partial state; double finish and late append do not reopen recording. No devices/provider used.")
  }
}
