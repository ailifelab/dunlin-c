import AVFoundation
import CoreImage
import ImageIO

final class MediaCamera: NSObject, AVCaptureVideoDataOutputSampleBufferDelegate, @unchecked Sendable {
  private let queue = DispatchQueue(label: "org.dunlin.media.camera", qos: .userInitiated)
  private let session = AVCaptureSession()
  private let context = CIContext(options: [.cacheIntermediates: false])
  private var lastFrame: Double = 0
  private var lastRecorded:Double = 0
  var recordingFrame:(@Sendable(Data)->Void)?
  private var configured = false
  private let deviceID:String
  init(deviceID:String) {self.deviceID=deviceID;super.init()}
  var frame: (@Sendable (Data) -> Void)?
  var failed: (@Sendable () -> Void)?
  func enable(_ enabled: Bool) {
    queue.async { [self] in
      if !enabled {session.stopRunning();return}
      if !configured {
        guard let camera = (deviceID.isEmpty ? AVCaptureDevice.default(for:.video):MediaDevices.cameras.first(where:{$0.uniqueID==deviceID})), let input = try? AVCaptureDeviceInput(device:camera), session.canAddInput(input) else {failed?();return}
        session.beginConfiguration();session.sessionPreset = .vga640x480;session.addInput(input)
        let output = AVCaptureVideoDataOutput();output.alwaysDiscardsLateVideoFrames = true
        output.videoSettings = [kCVPixelBufferPixelFormatTypeKey as String:kCVPixelFormatType_32BGRA]
        guard session.canAddOutput(output) else {session.commitConfiguration();failed?();return}
        output.setSampleBufferDelegate(self,queue:queue);session.addOutput(output);session.commitConfiguration();configured = true
      }
      session.startRunning()
    }
  }
  func stop() {queue.sync {session.stopRunning()}}
  func captureOutput(_ output:AVCaptureOutput,didOutput buffer:CMSampleBuffer,from connection:AVCaptureConnection) {
    let now = ProcessInfo.processInfo.systemUptime
    let sample=now-lastFrame>=2,record=recordingFrame != nil && now-lastRecorded>=1.0/15.0
    guard sample || record,let pixels = CMSampleBufferGetImageBuffer(buffer) else {return}
    let image = CIImage(cvPixelBuffer:pixels)
    guard let data = context.jpegRepresentation(of:image,colorSpace:CGColorSpaceCreateDeviceRGB(),options:[kCGImageDestinationLossyCompressionQuality as CIImageRepresentationOption:0.65]),data.count<=524288 else {failed?();return}
    if record {lastRecorded=now;recordingFrame?(data)}
    if sample {lastFrame=now;frame?(data)}
  }
}
