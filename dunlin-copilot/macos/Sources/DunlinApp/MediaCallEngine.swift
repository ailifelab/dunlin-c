import AppKit
import AVFoundation
import CDesktop

// Platform device owner. Protocol, event validation and transport remain in C.
// All C calls belong to worker; AVAudioEngine/player belong to audioQueue.
final class MediaCallEngine: NSObject, @unchecked Sendable {
  private let worker = DispatchQueue(label: "org.dunlin.media.transport", qos: .userInitiated)
  private let audioQueue = DispatchQueue(label: "org.dunlin.media.audio", qos: .userInitiated)
  private let lock = NSLock()
  private var cancelled = false
  private var muted = false
  private var pending: [Data] = []
  private var pendingFrame: Data?
  private var camera: MediaCamera?
  private var transcriber:MediaLocalTranscriber?
  private var video = false
  private var inputDevice = "",outputDevice = "",cameraDevice = ""
  private var cameraEnabled = false
  private var pendingBytes = 0
  private var engine: AVAudioEngine?
  private var player: AVAudioPlayerNode?
  private var currentItem = ""
  private var segments: [(item: String, start: Int64, end: Int64)] = []
  private var scheduledFrames: Int64 = 0
  private var interrupt: (String, UInt32)?
  var originalRecording:(@Sendable ()->MediaOriginalRecording?)?
  private var recording:MediaOriginalRecording?
  var event: (@Sendable (String, Data) -> Void)?
  func stop() { lock.lock(); cancelled = true; lock.unlock() }
  func mute(_ value: Bool) { lock.lock(); muted = value; if value { pending.removeAll(); pendingBytes = 0 }; lock.unlock() }
  private var stopped: Bool { lock.lock(); defer {lock.unlock()}; return cancelled }
  func enableCamera(_ enabled: Bool) {lock.lock();cameraEnabled = enabled;if !enabled {pendingFrame = nil};let device = camera;lock.unlock();device?.enable(enabled)}
  func speaker(_ enabled: Bool) {audioQueue.async {self.player?.volume = enabled ? 1:0}}
  func start(settings: String, secret: String, video: Bool = false, input: String = "", output: String = "", camera: String = "", localASR:Bool = false, language:String = "zh-CN") {
    inputDevice = input; outputDevice = output; cameraDevice = camera
    self.video = video; self.cameraEnabled = video
    worker.async { [self] in
      defer {
        lock.lock();cancelled=true;let device=self.camera;self.camera=nil;lock.unlock();device?.stop()
        audioQueue.sync {self.stopDevices()}
        transcriber?.finish();transcriber=nil
        recording?.finish();recording=nil;event?("closed",Data())
      }
      if let originalRecording {
        guard let recorder=originalRecording() else {event?("error",Data("{\"code\":\"recording_unavailable\"}".utf8));return}
        recording=recorder;recorder.warning = { [weak self] in self?.event?("recording.partial",Data()) }
      }
      if localASR {
        guard let adapter=MediaLocalTranscriber(locale:language) else {event?("error",Data("{\"code\":\"local_asr_unavailable\"}".utf8));return}
        transcriber=adapter;adapter.event = { [weak self] kind,data in self?.event?(kind,data) }
      }
      var hooks = dl_realtime_hooks(user: Unmanaged.passUnretained(self).toOpaque(), cancelled: { context in
        guard let context else { return 1 }
        return Unmanaged<MediaCallEngine>.fromOpaque(context).takeUnretainedValue().stopped ? 1 : 0
      }, event: { context, kind, bytes, count in
        guard let context, let kind else { return }
        let owner = Unmanaged<MediaCallEngine>.fromOpaque(context).takeUnretainedValue()
        owner.received(String(cString: kind), bytes.map { Data(bytes: $0, count: count) } ?? Data())
      })
      guard let session = dl_realtime_open(settings, secret, &hooks) else {
        if !stopped {event?("error", Data("{\"code\":\"connection_failed\"}".utf8))}; return
      }
      defer {
        dl_realtime_close(session)
        lock.lock(); pending.removeAll(); pendingBytes = 0; lock.unlock()

      }
      while !stopped {
        guard dl_realtime_poll(session) != 0 else { break }
        lock.lock(); let packets = pending; let frame = pendingFrame; pendingFrame = nil; pending.removeAll(); pendingBytes = 0; lock.unlock()
        var valid = true
        for data in packets {
          if stopped { break }
          if data.withUnsafeBytes({ dl_realtime_audio(session, $0.baseAddress, $0.count) }) == 0 { valid = false; break }
          recording?.append(1,data)
          transcriber?.append(data)
        }
        if let frame {
          lock.lock(); let enabled = cameraEnabled; lock.unlock()
          if enabled && frame.withUnsafeBytes({dl_realtime_image(session,$0.baseAddress,$0.count)}) == 0 {valid = false}
          else if enabled {event?("camera.frame",frame)}
        }
        if let item = interrupt { interrupt = nil; if dl_realtime_interrupt(session, item.0, item.1) == 0 {valid = false} }
        if !valid {break}
        Thread.sleep(forTimeInterval: 0.005)
      }
    }
  }
  private func received(_ kind: String, _ data: Data) {
    if stopped {return}
    switch kind {
    case "final":
      if transcriber != nil,let value=try? JSONSerialization.jsonObject(with:data) as? [String:String],value["role"]=="user" {return}
    case "gap":
      if let transcriber,let value=try? JSONSerialization.jsonObject(with:data) as? [String:String],value["role"]=="user",let item=value["item"] {transcriber.seal(item:item);return}
    case "ready":
      do { try audioQueue.sync { try self.startDevices() } }
      catch { event?("error", Data("{\"code\":\"audio_device_failed\"}".utf8)); stop(); return }
      if video {
        let camera = MediaCamera(deviceID:cameraDevice);lock.lock();self.camera = camera;let enabled = cameraEnabled;lock.unlock()
        camera.frame = { [weak self] data in
          guard let self else {return};self.lock.lock();if self.cameraEnabled && !self.cancelled {self.pendingFrame = data};self.lock.unlock()
        }
        if let recording {camera.recordingFrame = {data in recording.append(3,data)}}
        camera.failed = { [weak self] in self?.event?("error",Data("{\"code\":\"camera_failed\"}".utf8));self?.stop() }
        camera.enable(enabled)
      }
    case "audio.item":
      if let value = try? JSONSerialization.jsonObject(with: data) as? [String: String] { currentItem = value["item"] ?? "" }
      return
    case "audio":
      recording?.append(2,data)
      let item = currentItem
      let ok = audioQueue.sync { self.play(data, item: item) }
      if !ok {event?("error", Data("{\"code\":\"playback_overflow\"}".utf8));stop()}
      return
    case "interrupted":
      interrupt = audioQueue.sync {
        guard let player = self.player else {return nil}
        let played = player.lastRenderTime.flatMap {player.playerTime(forNodeTime: $0)}?.sampleTime ?? 0
        let item = self.segments.first { $0.end > played }
        player.stop(); player.play(); self.segments.removeAll(); self.scheduledFrames = 0
        return item.map {($0.item, UInt32(max(0, played - $0.start) * 1000 / 24000))}
      }
    default: break
    }
    event?(kind, data)
  }
  private func startDevices() throws {
    let engine = AVAudioEngine(), player = AVAudioPlayerNode()
    let input = engine.inputNode
    try MediaDevices.select(inputDevice,node:input,input:true)
    try MediaDevices.select(outputDevice,node:engine.outputNode,input:false)
    try input.setVoiceProcessingEnabled(true)
    let format = input.outputFormat(forBus: 0)
    guard format.sampleRate > 0, format.channelCount > 0,
      let pcm = AVAudioFormat(commonFormat: .pcmFormatFloat32, sampleRate: 24000, channels: 1, interleaved: false),
      let converter = AVAudioConverter(from: format, to: pcm) else {throw NSError(domain: "Audio", code: 1)}
    engine.attach(player); engine.connect(player, to: engine.mainMixerNode, format: pcm)
    self.engine = engine; self.player = player
    input.installTap(onBus: 0, bufferSize: 2048, format: format) { [weak self] buffer, _ in
      guard let self, !self.stopped else { return }
      self.lock.lock(); let mute = self.muted; self.lock.unlock(); if mute {return}
      let capacity = AVAudioFrameCount(ceil(Double(buffer.frameLength) * 24000 / format.sampleRate) + 32)
      guard let converted = AVAudioPCMBuffer(pcmFormat: pcm, frameCapacity: capacity) else {return}
      var supplied = false, conversionError: NSError?
      let result = converter.convert(to: converted, error: &conversionError) { _, status in
        if supplied {status.pointee = .noDataNow; return nil}
        supplied = true; status.pointee = .haveData; return buffer
      }
      guard result != .error, conversionError == nil, let samples = converted.floatChannelData?[0] else {self.stop();return}
      var data = Data(capacity: Int(converted.frameLength) * 2)
      for index in 0..<Int(converted.frameLength) {
        var value = Int16(max(-32768, min(32767, Double(samples[index]) * 32767))).littleEndian
        withUnsafeBytes(of: &value) {data.append(contentsOf: $0)}
      }
      self.lock.lock()
      if !self.cancelled && !self.muted && !data.isEmpty {
        if self.pendingBytes + data.count > 192000 {self.cancelled = true;self.event?("error",Data("{\"code\":\"capture_overflow\"}".utf8))}
        else {self.pending.append(data); self.pendingBytes += data.count}
      }
      self.lock.unlock()
    }
    engine.prepare(); try engine.start(); player.play()
  }
  private func play(_ data: Data, item: String) -> Bool {
    guard let player, let pcm = AVAudioFormat(commonFormat: .pcmFormatFloat32, sampleRate: 24000, channels: 1, interleaved: false) else {return false}
    let frames = data.count / 2
    let played = player.lastRenderTime.flatMap { player.playerTime(forNodeTime: $0) }?.sampleTime ?? 0
    segments.removeAll { $0.end <= played }
    guard scheduledFrames - played + Int64(frames) <= 480000,
      let buffer = AVAudioPCMBuffer(pcmFormat: pcm, frameCapacity: AVAudioFrameCount(frames)),
      let out = buffer.floatChannelData?[0] else {return false}
    buffer.frameLength = AVAudioFrameCount(frames)
    data.withUnsafeBytes { bytes in
      for i in 0..<frames {out[i] = Float(Int16(littleEndian: bytes.loadUnaligned(fromByteOffset: i * 2, as: Int16.self))) / 32768}
    }
    if let last = segments.last, last.item == item {segments[segments.count - 1].end += Int64(frames)}
    else {segments.append((item, scheduledFrames, scheduledFrames + Int64(frames)))}
    scheduledFrames += Int64(frames); player.scheduleBuffer(buffer); return true
  }
  private func stopDevices() {
    engine?.inputNode.removeTap(onBus: 0); player?.stop(); engine?.stop(); player = nil; engine = nil
    segments.removeAll(); scheduledFrames = 0
  }
}
