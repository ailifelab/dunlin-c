import AVFoundation
import AudioToolbox
import CoreAudio
struct MediaAudioDevice: Identifiable {
  let id:String,name:String,device:AudioDeviceID
}
enum MediaDevices {
  static func audio(input:Bool)->[MediaAudioDevice] {
    var address=AudioObjectPropertyAddress(mSelector:kAudioHardwarePropertyDevices,mScope:kAudioObjectPropertyScopeGlobal,mElement:kAudioObjectPropertyElementMain)
    var size:UInt32=0
    guard AudioObjectGetPropertyDataSize(AudioObjectID(kAudioObjectSystemObject),&address,0,nil,&size)==noErr else {return []}
    var ids=[AudioDeviceID](repeating:0,count:Int(size)/MemoryLayout<AudioDeviceID>.size)
    guard AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject),&address,0,nil,&size,&ids)==noErr else {return []}
    return ids.compactMap {device in
      var streams=AudioObjectPropertyAddress(mSelector:kAudioDevicePropertyStreams,mScope:input ? kAudioDevicePropertyScopeInput:kAudioDevicePropertyScopeOutput,mElement:kAudioObjectPropertyElementMain)
      var bytes:UInt32=0
      guard AudioObjectGetPropertyDataSize(device,&streams,0,nil,&bytes)==noErr,bytes>0 else {return nil}
      func string(_ selector:AudioObjectPropertySelector)->String? {
        var a=AudioObjectPropertyAddress(mSelector:selector,mScope:kAudioObjectPropertyScopeGlobal,mElement:kAudioObjectPropertyElementMain)
        var value:CFString="" as CFString;var size=UInt32(MemoryLayout<CFString>.size)
        return AudioObjectGetPropertyData(device,&a,0,nil,&size,&value)==noErr ? value as String:nil
      }
      guard let id=string(kAudioDevicePropertyDeviceUID),let name=string(kAudioObjectPropertyName) else {return nil}
      return MediaAudioDevice(id:id,name:name,device:device)
    }
  }
  static var cameras:[AVCaptureDevice] {
    AVCaptureDevice.DiscoverySession(deviceTypes:[.builtInWideAngleCamera,.external,.continuityCamera],mediaType:.video,position:.unspecified).devices
  }
  static func select(_ uid:String,node:AVAudioIONode,input:Bool)throws {
    if uid.isEmpty {return}
    guard var device=audio(input:input).first(where:{$0.id==uid})?.device,let unit=node.audioUnit else {throw NSError(domain:"DunlinMediaDevice",code:1)}
    let status=AudioUnitSetProperty(unit,kAudioOutputUnitProperty_CurrentDevice,kAudioUnitScope_Global,0,&device,UInt32(MemoryLayout<AudioDeviceID>.size))
    if status != noErr {throw NSError(domain:NSOSStatusErrorDomain,code:Int(status))}
  }
}
