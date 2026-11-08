import AppKit
import CDesktop
import Security
import SwiftUI

// Event/approval handlers are configured before enqueue; only cancellation mutates across queues.
final class Callbacks: @unchecked Sendable {
  let lock = NSLock()
  var stopped = false
  var approve: (String, String) -> Int32 = { _, _ in 0 }
  var event: (String, String) -> Void = { _, _ in }
  func cancel() {
    lock.lock()
    stopped = true
    lock.unlock()
  }
  func isCancelled() -> Bool {
    lock.lock()
    defer { lock.unlock() }
    return stopped
  }
}
enum ModelCredentialError: Error, LocalizedError {
  case cancelled, denied, unavailable(OSStatus), invalid
  var errorDescription: String? {
    switch self {
    case .cancelled: return "已取消钥匙串授权，未向模型发送请求。"
    case .denied: return "钥匙串授权未通过，未向模型发送请求。"
    case .unavailable: return "钥匙串暂不可用，请解锁后重试；不会改用其他凭据。"
    case .invalid: return "密钥格式无效，请检查长度和换行字符。"
    }
  }
}
func readCredential(_ id: String) -> Result<String?,ModelCredentialError> {
  let q: [String: Any] = [
    kSecClass as String: kSecClassGenericPassword,
    kSecAttrService as String: "org.dunlin.desktop.models", kSecAttrAccount as String: id,
  ]
  var read = q
  read[kSecReturnData as String] = true
  var item: CFTypeRef?
  let status=SecItemCopyMatching(read as CFDictionary,&item)
  if status==errSecItemNotFound{return .success(nil)}
  if status==errSecUserCanceled{return .failure(.cancelled)}
  if status==errSecAuthFailed{return .failure(.denied)}
  guard status==errSecSuccess else{return .failure(.unavailable(status))}
  guard let data=item as? Data,let value=String(data:data,encoding:.utf8) else{return .failure(.invalid)}
  return .success(value)
}
func keychain(_ id:String)->String { (try? readCredential(id).get()) ?? "" }
func modelCredential(_ profile:ModelProfile)->Result<String,ModelCredentialError> {
  if profile.authentication=="none" {return .success("")}
  switch readCredential(profile.keychainID) {
  case .failure(let error): return .failure(error)
  case .success(let saved):
    let value=saved ?? ""
    guard value.utf8.count<=16384,!value.contains("\n"),!value.contains("\r"),!value.utf8.contains(0) else{return .failure(.invalid)}
    return .success(value)
  }
}

func saveCredential(_ id: String, _ value: String) -> Bool {
  guard value.utf8.count<=16384,!value.contains("\n"),!value.contains("\r"),!value.utf8.contains(0) else{return false}
  let query: [String: Any] = [
    kSecClass as String: kSecClassGenericPassword,
    kSecAttrService as String: "org.dunlin.desktop.models", kSecAttrAccount as String: id,
  ]
  if value.isEmpty {
    let result = SecItemDelete(query as CFDictionary)
    return result == errSecSuccess || result == errSecItemNotFound
  }
  let data = Data(value.utf8)
  let updated = SecItemUpdate(
    query as CFDictionary, [kSecValueData as String: data] as CFDictionary)
  if updated == errSecSuccess { return true }
  guard updated == errSecItemNotFound else { return false }
  var add = query
  add[kSecValueData as String] = data
  return SecItemAdd(add as CFDictionary, nil) == errSecSuccess
}

// File previews must obey the file tool's no-symlink and bounded UTF-8 policy.
func readProjectText(_ url: URL) throws -> String {
  let fd = open(url.path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC)
  guard fd >= 0 else { throw NSError(domain: NSPOSIXErrorDomain, code: Int(errno)) }
  let handle = FileHandle(fileDescriptor: fd, closeOnDealloc: true)
  var info = stat()
  guard fstat(fd, &info) == 0, (info.st_mode & S_IFMT) == S_IFREG, info.st_size <= 65536 else {
    throw NSError(domain: "Dunlin", code: 1)
  }
  let data = try handle.read(upToCount: 65537) ?? Data()
  guard data.count <= 65536, let text = String(data: data, encoding: .utf8) else {
    throw NSError(domain: "Dunlin", code: 2)
  }
  return text
}

// The C traversal rejects symlinks in every path component, including previews
// shown before a model tool has executed. No main-queue database access required.
func projectPreview(root: String, path: String) -> ProjectDocument? {
  guard let raw = dc_preview_file(root, path) else { return nil }
  defer { dc_free(raw) }
  return try? JSONDecoder().decode(ProjectDocument.self, from: Data(String(cString: raw).utf8))
}
