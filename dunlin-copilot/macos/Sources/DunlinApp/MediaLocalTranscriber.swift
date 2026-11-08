import AVFoundation
import Speech

// Optional platform adapter. C still owns the canonical transcript and retention.
// Audio exists only in bounded memory; no remote Speech fallback is permitted.
final class MediaLocalTranscriber:@unchecked Sendable {
  private let queue=DispatchQueue(label:"org.dunlin.media.local-asr")
  private let recognizer:SFSpeechRecognizer
  private var request:SFSpeechAudioBufferRecognitionRequest?
  private var task:SFSpeechRecognitionTask?
  private var current=Data()
  private var pending:[(String,Data)]=[]
  private var activeID:String?
  private var seen:Set<String>=[]
  private var closing=false
  private var disabled=false
  private var closeSignal:DispatchSemaphore?
  private let ingress=NSLock()
  private var queuedBytes=0
  var event:(@Sendable (String,Data)->Void)?
  init?(locale:String) {
    guard SFSpeechRecognizer.authorizationStatus() == .authorized,
      let recognizer=SFSpeechRecognizer(locale:Locale(identifier:locale)),
      recognizer.supportsOnDeviceRecognition,recognizer.isAvailable else{return nil}
    self.recognizer=recognizer
  }
  static func available(locale:String)->Bool {
    guard let recognizer=SFSpeechRecognizer(locale:Locale(identifier:locale)) else{return false}
    return recognizer.supportsOnDeviceRecognition && recognizer.isAvailable
  }
  func append(_ data:Data) {
    ingress.lock()
    let admitted=queuedBytes+data.count<=192000
    if admitted {queuedBytes+=data.count}
    ingress.unlock()
    guard admitted else {queue.async{self.fail()};return}
    queue.async {
      self.ingress.lock();self.queuedBytes-=data.count;self.ingress.unlock()
      guard !self.closing,!self.disabled else{return}
      if self.current.count+data.count>2_880_000 {self.fail();return}
      self.current.append(data)
    }
  }
  // Provider VAD finalizes an input item. Recognition is explicitly approximate;
  // receipt boundaries may include trailing silence received during network delay.
  func seal(item:String) {
    queue.async {
      guard !self.closing,!self.seen.contains(item) else{return}
      guard self.seen.count<4096 else{self.fail();return}
      self.seen.insert(item)
      if self.disabled {self.emit("gap",item:item,text:"");return}
      self.enqueue(item:item);self.pump()
    }
  }
  private func enqueue(item:String) {
    guard !current.isEmpty else{emit("gap",item:item,text:"");return}
    guard pending.count<2 else{fail();return}
    pending.append((item,current));current=Data()
  }
  private func pump() {
    guard activeID==nil,!disabled,!pending.isEmpty else {finishIfIdle();return}
    let (id,data)=pending.removeFirst();activeID=id
    guard let format=AVAudioFormat(commonFormat:.pcmFormatFloat32,sampleRate:24000,channels:1,interleaved:false),
      let buffer=AVAudioPCMBuffer(pcmFormat:format,frameCapacity:AVAudioFrameCount(data.count/2)),
      let samples=buffer.floatChannelData?[0] else{fail();return}
    buffer.frameLength=buffer.frameCapacity
    data.withUnsafeBytes {raw in
      for index in 0..<Int(buffer.frameLength) {samples[index]=Float(Int16(littleEndian:raw.loadUnaligned(fromByteOffset:index*2,as:Int16.self)))/32768}
    }
    let request=SFSpeechAudioBufferRecognitionRequest()
    request.requiresOnDeviceRecognition=true;request.shouldReportPartialResults=false
    self.request=request
    task=recognizer.recognitionTask(with:request) {[weak self] result,error in
      guard let self else{return}
      let final=result?.isFinal == true
      let text=final ? result?.bestTranscription.formattedString:nil
      guard final || error != nil else{return}
      self.queue.async {
        guard self.activeID==id else{return}
        if let text,!text.isEmpty {self.emit("final",item:id,text:text)}else{self.emit("gap",item:id,text:"")}
        self.task=nil;self.request=nil;self.activeID=nil
        self.pump()
      }
    }
    request.append(buffer);request.endAudio()
    queue.asyncAfter(deadline:.now()+15) {
      guard self.activeID==id else{return}
      self.fail()
    }
  }
  private func emit(_ kind:String,item:String,text:String) {
    let body=["item":item,"role":"user","source":"local_asr","text":text]
    if let data=try? JSONSerialization.data(withJSONObject:body) {event?(kind,data)}
  }
  private func fail() {
    guard !disabled else{return};disabled=true
    if let activeID {emit("gap",item:activeID,text:"")}
    for (id,_) in pending {emit("gap",item:id,text:"")}
    if !current.isEmpty {emit("gap",item:"local-"+UUID().uuidString,text:"")}
    activeID=nil;task?.cancel();task=nil;request=nil;pending=[];current=Data()
    event?("transcription.failed",Data());finishIfIdle()
  }
  private func finishIfIdle() {
    if closing && activeID==nil && pending.isEmpty {closeSignal?.signal();closeSignal=nil}
  }
  // Called only by the transport worker, never by the UI/audio callback thread.
  func finish() {
    let signal=DispatchSemaphore(value:0)
    queue.async {
      self.closing=true;self.closeSignal=signal
      if !self.disabled,!self.current.isEmpty {self.enqueue(item:"local-"+UUID().uuidString)}
      self.pump();self.finishIfIdle()
    }
    if signal.wait(timeout:.now()+5) == .timedOut {
      queue.sync {self.fail();self.finishIfIdle()}
    }
  }
}
