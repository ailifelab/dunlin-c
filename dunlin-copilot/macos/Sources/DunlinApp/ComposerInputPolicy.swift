import Foundation

struct ComposerModifiers: OptionSet {
  let rawValue: Int
  static let shift = Self(rawValue: 1 << 0)
  static let command = Self(rawValue: 1 << 1)
  static let option = Self(rawValue: 1 << 2)
  static let control = Self(rawValue: 1 << 3)
}
enum ComposerInputPolicy {
  static func sendsReturn(
    policy: ChatSendKey, modifiers: ComposerModifiers, markedText: Bool, canSubmit: Bool
  ) -> Bool {
    guard !markedText, canSubmit, modifiers.intersection([.shift, .option, .control]).isEmpty else { return false }
    return policy == .returnKey || modifiers.contains(.command)
  }
}
