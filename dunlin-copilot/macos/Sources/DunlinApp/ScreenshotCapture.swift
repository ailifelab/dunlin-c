import AppKit
import ImageIO
@preconcurrency import ScreenCaptureKit
import UniformTypeIdentifiers

// Presentation/capture belongs to the macOS adapter. Validation, durable
// resources, ownership and model projection remain in independent C services.
@MainActor final class ScreenshotCapture: NSObject, SCContentSharingPickerObserver {
  private var completion: ((Result<Data, Error>?) -> Void)?
  private var capturing = false
  init(completion: @escaping (Result<Data, Error>?) -> Void) { self.completion = completion }
  func start() {
    let picker = SCContentSharingPicker.shared
    var config = SCContentSharingPickerConfiguration()
    config.allowedPickerModes = [.singleWindow, .singleDisplay]
    config.allowsChangingSelectedContent = false
    picker.defaultConfiguration = config
    picker.add(self)
    picker.isActive = true
    picker.present()
  }
  func cancel() { finish(nil) }
  private func finish(_ result: Result<Data, Error>?) {
    guard let completion else { return }
    self.completion = nil
    SCContentSharingPicker.shared.remove(self)
    SCContentSharingPicker.shared.isActive = false
    completion(result)
  }
  nonisolated func contentSharingPicker(_ picker: SCContentSharingPicker, didCancelFor stream: SCStream?) {
    Task { @MainActor in if !self.capturing { self.finish(nil) } }
  }
  nonisolated func contentSharingPickerStartDidFailWithError(_ error: any Error) {
    Task { @MainActor in self.finish(.failure(error)) }
  }
  nonisolated func contentSharingPicker(_ picker: SCContentSharingPicker,
    didUpdateWith filter: SCContentFilter, for stream: SCStream?) {
    Task { @MainActor in
      guard self.completion != nil, !self.capturing else { return }
      self.capturing = true
      let rect = filter.contentRect
      let scale = Double(filter.pointPixelScale)
      let width = Double(rect.width) * scale
      let height = Double(rect.height) * scale
      guard width.isFinite, height.isFinite, width > 0, height > 0 else {
        self.finish(.failure(NSError(domain: "DunlinCapture", code: 1,
          userInfo: [NSLocalizedDescriptionKey: "所选窗口或屏幕暂不可捕获。请重新选择。"])))
        return
      }
      let ratio = min(1, 1024 / max(width, height))
      let config = SCScreenshotConfiguration()
      config.width = max(1, Int(width * ratio))
      config.height = max(1, Int(height * ratio))
      config.showsCursor = false
      config.ignoreShadows = true
      config.dynamicRange = .sdr
      config.contentType = UTType.png as UTTypeReference
      SCScreenshotManager.captureScreenshot(contentFilter: filter, configuration: config) { output, error in
        if let error { Task { @MainActor in self.finish(.failure(error)) }; return }
        guard let image = output?.sdrImage else {
          Task { @MainActor in self.finish(.failure(NSError(domain: "DunlinCapture", code: 2,
            userInfo: [NSLocalizedDescriptionKey: "系统没有返回截图。原聊天保持不变。"]))) }
          return
        }
        let data = NSMutableData()
        guard let destination = CGImageDestinationCreateWithData(data, UTType.png.identifier as CFString, 1, nil) else {
          Task { @MainActor in self.finish(.failure(NSError(domain: "DunlinCapture", code: 3))) }; return
        }
        CGImageDestinationAddImage(destination, image, nil)
        guard CGImageDestinationFinalize(destination) else {
          Task { @MainActor in self.finish(.failure(NSError(domain: "DunlinCapture", code: 4))) }; return
        }
        let bytes = data as Data
        Task { @MainActor in self.finish(.success(bytes)) }
      }
    }
  }
}
