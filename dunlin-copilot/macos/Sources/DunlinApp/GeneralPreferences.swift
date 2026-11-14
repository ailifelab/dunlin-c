import Foundation

enum ChatTextSize: String, Codable, CaseIterable {
  case small, standard, large
  var title: String { switch self { case .small: return "较小"; case .standard: return "标准"; case .large: return "较大" } }
  var points: Double { switch self { case .small: return 13; case .standard: return 14; case .large: return 18 } }
}
enum ChatSendKey: String, Codable, CaseIterable {
  case returnKey = "return"
  case commandReturn
  var title: String { self == .returnKey ? "Return 发送" : "⌘ Return 发送" }
  var hint: String { self == .returnKey ? "Shift + Return 换行" : "Return 换行 · ⌘ Return 发送" }
}
struct GeneralPreferences: Codable, Equatable {
  var textSize: ChatTextSize = .standard
  var sendKey: ChatSendKey = .returnKey
  var notifyOnCompletion = false
  init() {}
  enum CodingKeys: String, CodingKey { case textSize, sendKey, notifyOnCompletion }
  init(from decoder: Decoder) throws {
    let c = try decoder.container(keyedBy: CodingKeys.self)
    textSize = (try? c.decode(ChatTextSize.self, forKey: .textSize)) ?? .standard
    sendKey = (try? c.decode(ChatSendKey.self, forKey: .sendKey)) ?? .returnKey
    notifyOnCompletion = (try? c.decode(Bool.self, forKey: .notifyOnCompletion)) ?? false
  }
}
