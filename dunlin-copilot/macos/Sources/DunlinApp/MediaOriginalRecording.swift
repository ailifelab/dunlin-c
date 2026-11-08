import Foundation
import CDesktop

// Device code supplies encoded packets. The C owner creates private storage,
// validates the stream and commits its final state; no file paths enter Swift.
final class MediaOriginalRecording:@unchecked Sendable {
  private let worker=DispatchQueue(label:"org.dunlin.media.recording",qos:.utility)
  private let owner:DispatchQueue,store:NativeStoreReference
  private let lock=NSLock()
  private var capture:OpaquePointer?
  private var pending=0,closing=false,failed=false
  private let started=ProcessInfo.processInfo.systemUptime
  var warning:(@Sendable()->Void)?
  init?(store:NativeStoreReference,owner:DispatchQueue,call:String){
    self.store=store;self.owner=owner
    capture=owner.sync {dc_media_recording_open(store.raw,call,1_073_741_824)}
    if capture==nil{return nil}
  }
  func append(_ track:Int32,_ data:Data){
    let stamp=UInt64(max(0,ProcessInfo.processInfo.systemUptime-started)*1000)
    lock.lock()
    guard !closing,!failed else{lock.unlock();return}
    if pending+data.count>2_097_152 {failed=true;lock.unlock();warning?();return}
    pending+=data.count;lock.unlock()
    worker.async { [self] in
      let result=capture.map {capture in data.withUnsafeBytes {dc_media_recording_append(capture,track,stamp,$0.baseAddress,$0.count)}} ?? 1
      lock.lock();pending-=data.count;let report=result != 0 && !failed;if result != 0 {failed=true};lock.unlock()
      if report {warning?()}
    }
  }
  // Called after capture devices have stopped. Drains bounded pending I/O before
  // the call's ended event, so playback never sees a file still being written.
  func finish(){
    lock.lock();closing=true;let wasFailed=failed;lock.unlock()
    worker.sync {
      guard let capture else{return};self.capture=nil
      if wasFailed { _ = dc_media_recording_append(capture,0,0,nil,0) }
      let valid:Bool=owner.sync {
        guard let raw=dc_media_recording_finish(store.raw,capture) else{return false}
        defer{dc_free(raw)}
        guard let data=String(cString:raw).data(using:.utf8),let value=try? JSONSerialization.jsonObject(with:data) as? [String:Any] else{return false}
        return value["ok"] as? Bool == true && (value["data"] as? [String:Any])?["state"] as? String == "ready"
      }
      if !valid {warning?()}
    }
  }
}
