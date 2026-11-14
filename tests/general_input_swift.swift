import Foundation
@main enum GeneralInputChecks {
 static func main() throws {
  let decoder = JSONDecoder(), encoder = JSONEncoder()
  let defaults = try decoder.decode(GeneralPreferences.self, from: Data("{}".utf8))
  assert(defaults == GeneralPreferences() && defaults.sendKey == .returnKey && defaults.textSize == .standard && !defaults.notifyOnCompletion)
  let repaired = try decoder.decode(GeneralPreferences.self, from: Data(#"{"sendKey":"unsupported","textSize":false,"notifyOnCompletion":"yes"}"#.utf8))
  assert(repaired == defaults)
  var personal = defaults
  personal.textSize = .large; personal.sendKey = .commandReturn; personal.notifyOnCompletion = true
  let saved = try encoder.encode(personal), loaded = try decoder.decode(GeneralPreferences.self, from: saved)
  assert(loaded == personal && loaded != defaults)
  // Separate preference values do not share mutation or carry another identity's settings.
  var other = try decoder.decode(GeneralPreferences.self, from: Data("{}".utf8))
  other.textSize = .small
  assert(personal.textSize == .large && other.sendKey == .returnKey && !other.notifyOnCompletion)
  assert(Set(ChatTextSize.allCases.map(\.points)) == Set([13.0,14.0,18.0]))
  for policy in ChatSendKey.allCases {
   for raw in 0..<16 {
    let modifiers = ComposerModifiers(rawValue: raw)
    assert(!ComposerInputPolicy.sendsReturn(policy: policy, modifiers: modifiers, markedText: true, canSubmit: true))
    assert(!ComposerInputPolicy.sendsReturn(policy: policy, modifiers: modifiers, markedText: false, canSubmit: false))
   }
  }
  assert(ComposerInputPolicy.sendsReturn(policy: .returnKey, modifiers: [], markedText: false, canSubmit: true))
  assert(ComposerInputPolicy.sendsReturn(policy: .returnKey, modifiers: [.command], markedText: false, canSubmit: true))
  assert(!ComposerInputPolicy.sendsReturn(policy: .returnKey, modifiers: [.shift], markedText: false, canSubmit: true))
  assert(!ComposerInputPolicy.sendsReturn(policy: .returnKey, modifiers: [.option], markedText: false, canSubmit: true))
  assert(!ComposerInputPolicy.sendsReturn(policy: .returnKey, modifiers: [.control], markedText: false, canSubmit: true))
  assert(!ComposerInputPolicy.sendsReturn(policy: .commandReturn, modifiers: [], markedText: false, canSubmit: true))
  assert(ComposerInputPolicy.sendsReturn(policy: .commandReturn, modifiers: [.command], markedText: false, canSubmit: true))
  assert(!ComposerInputPolicy.sendsReturn(policy: .commandReturn, modifiers: [.command,.shift], markedText: false, canSubmit: true))
  print("PASS saved preference defaults/unknown-field recovery/round-trip/isolated values; marked-text and disabled-send never submit; Return/Cmd/Shift/Option/Control selection policy. Actual NSTextInputClient/IME, OS permission and visual behavior remain unverified.")
 }
}
