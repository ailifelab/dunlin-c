import SwiftUI
import AVFoundation
import ImageIO
import CDesktop

private final class MediaReplayReference:@unchecked Sendable {
  let raw:OpaquePointer
  init(_ raw:OpaquePointer){self.raw=raw}
  deinit{dc_media_recording_read_close(raw)}
}
struct OriginalRecordingInfo:Decodable {let state:String;let bytes:Int64?}
final class MediaRecordingPlayback:ObservableObject,@unchecked Sendable {
  @Published var status="尚未播放"
  @Published var seconds:Double=0
  @Published var image:NSImage?
  @Published var playing=false
  private let worker=DispatchQueue(label:"org.dunlin.media.replay",qos:.userInitiated)
  private let lock=NSLock()
  private var cancelled=false,pendingUI=false,latestImage:CGImage?,latestSeconds:Double=0
  private var engine:AVAudioEngine?,players:[AVAudioPlayerNode]=[]
  private var audioEnds=[0.0,0.0]
  private var anchor:Double?,firstStamp:UInt64=0,endAt:Double=0
  private var stopped:Bool {lock.lock();defer{lock.unlock()};return cancelled}
  func stop(){lock.lock();cancelled=true;lock.unlock()}
  func start(_ replay:OpaquePointer,output:String){
    lock.lock();cancelled=false;lock.unlock();playing=true;status="播放中";seconds=0;image=nil
    let reader=MediaReplayReference(replay)
    worker.async { [self,reader] in
      defer {engine?.stop();engine=nil;players=[];DispatchQueue.main.async {self.playing=false}}
      do {
        let e=AVAudioEngine();try MediaDevices.select(output,node:e.outputNode,input:false)
        let format=AVAudioFormat(commonFormat:.pcmFormatFloat32,sampleRate:24000,channels:1,interleaved:false)!
        players=(0..<2).map {_ in let p=AVAudioPlayerNode();e.attach(p);e.connect(p,to:e.mainMixerNode,format:format);return p};engine=e;try e.start();players.forEach{$0.play()}
      }catch{DispatchQueue.main.async {self.status="无法启动音频输出设备"};return}
      anchor=nil;endAt=0;audioEnds=[0,0]
      let result=dc_media_recording_scan(reader.raw,{context,track,stamp,bytes,count in
        guard let context,let bytes else{return 0}
        return Unmanaged<MediaRecordingPlayback>.fromOpaque(context).takeUnretainedValue().consume(track,stamp:stamp,data:Data(bytes:bytes,count:count)) ? 1:0
      },Unmanaged.passUnretained(self).toOpaque())
      if result==0,let anchor {while !stopped && ProcessInfo.processInfo.systemUptime<anchor+endAt {Thread.sleep(forTimeInterval:0.01)}}
      let text=stopped ? "已停止":result==0 ? "播放结束":"录制内容损坏或不完整，播放已停止"
      let duration=endAt
      DispatchQueue.main.async {self.status=text;if result==0 && text=="播放结束" {self.seconds=duration}}
    }
  }
  private func consume(_ track:Int32,stamp:UInt64,data:Data)->Bool {
    guard !stopped,stamp<=3_600_000 else{return false}
    if anchor==nil {anchor=ProcessInfo.processInfo.systemUptime;firstStamp=stamp}
    let position=Double(stamp>=firstStamp ? stamp-firstStamp:0)/1000
    while !stopped && ProcessInfo.processInfo.systemUptime<(anchor ?? 0)+position {Thread.sleep(forTimeInterval:0.01)}
    guard !stopped else{return false}
    if track==1 || track==2 {
      let slot=Int(track)-1
      while !stopped && audioEnds[slot]-(ProcessInfo.processInfo.systemUptime-(anchor ?? 0))>1 {Thread.sleep(forTimeInterval:0.01)}
      guard !stopped else{return false}
      let frames=data.count/2
      guard let format=AVAudioFormat(commonFormat:.pcmFormatFloat32,sampleRate:24000,channels:1,interleaved:false),let buffer=AVAudioPCMBuffer(pcmFormat:format,frameCapacity:AVAudioFrameCount(frames)),let samples=buffer.floatChannelData?[0] else{return false}
      buffer.frameLength=AVAudioFrameCount(frames)
      data.withUnsafeBytes {bytes in for i in 0..<frames {samples[i]=Float(Int16(littleEndian:bytes.loadUnaligned(fromByteOffset:i*2,as:Int16.self)))/32768}}
      players[slot].scheduleBuffer(buffer);audioEnds[slot]=max(audioEnds[slot],position)+Double(frames)/24000;endAt=max(endAt,audioEnds[slot])
    }else{endAt=max(endAt,position)}
    var frame:CGImage?
    if track==3 {
      guard let source=CGImageSourceCreateWithData(data as CFData,nil),let props=CGImageSourceCopyPropertiesAtIndex(source,0,nil) as? [CFString:Any],let width=props[kCGImagePropertyPixelWidth] as? Int,let height=props[kCGImagePropertyPixelHeight] as? Int,width>0,height>0,width<=4096,height<=4096,let decoded=CGImageSourceCreateThumbnailAtIndex(source,0,[kCGImageSourceCreateThumbnailFromImageAlways:true,kCGImageSourceThumbnailMaxPixelSize:1024] as CFDictionary) else{return false}
      frame=decoded
    }
    lock.lock();latestSeconds=position;if let frame {latestImage=frame};let update = !pendingUI;pendingUI=true;lock.unlock()
    if update {DispatchQueue.main.async { [self] in
      lock.lock();let frame=latestImage,time=latestSeconds;latestImage=nil;pendingUI=false;lock.unlock();seconds=time
      if let frame {image=NSImage(cgImage:frame,size:.zero)}
    }}
    return true
  }
}
struct MediaRecordingPlaybackView:View {
  @EnvironmentObject var store:WorkspaceStore
  @Environment(\.dismiss) private var dismiss
  @StateObject private var player=MediaRecordingPlayback()
  @State private var opening=false
  @State private var alive=true
  @State private var error=""
  let call:MediaCallRecord,info:OriginalRecordingInfo
  var body:some View {
    VStack(spacing:20){
      HStack{Text("原始媒体回看").font(.title2.bold());Spacer();Button("关闭"){player.stop();dismiss()}}
      if let image=player.image {Image(nsImage:image).resizable().scaledToFit().frame(maxHeight:320)}
      else {Image(systemName:call.mode=="video" ? "video":"waveform").font(.system(size:72)).frame(height:220).foregroundStyle(.secondary)}
      Text(player.status).font(.headline)
      Text(String(format:"%02d:%02d",Int(player.seconds)/60,Int(player.seconds)%60)).monospacedDigit()
      if info.state=="partial" {Text("这份录制不完整：达到容量限制或录制中断；未记录部分无法回放。").foregroundStyle(.orange)}
      Text("播放发送/接收的原始音频和本机摄像画面。服务端音轨可能包含通话时被打断、没有实际播放的声音；不能据此推断当时听到的内容。").font(.caption).foregroundStyle(.secondary)
      if !error.isEmpty{Text(error).foregroundStyle(.orange)}
      HStack{Button("播放"){open()}.disabled(player.playing||opening);Button("停止"){player.stop()}.disabled(!player.playing)}
    }.padding(28).frame(width:660,height:560).onDisappear {alive=false;player.stop()}
  }
  private func open(){
    guard let owner=store.handle else{return};opening=true;error=""
    let epoch=store.identityEpoch,output=store.mediaPreferences.outputDevice ?? ""
    store.queue.async {
      let replay=dc_media_recording_read_open(owner.raw,call.id)
      DispatchQueue.main.async {
        opening=false
        guard alive,store.identityEpoch==epoch else{if let replay{dc_media_recording_read_close(replay)};return}
        guard let replay else{error="录制已过期、未完成或不可读取。";return}
        player.start(replay,output:output)
      }
    }
  }
}
