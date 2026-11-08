import CDesktop

// Editor choices are bounded by the same C authority as send/save validation.
// A bad draft selection must not erase otherwise valid capability bounds.
enum ModelContextBudget {
  static func limits(_ capabilities: dc_context_capabilities) -> dc_context_range? {
    var caps = capabilities
    caps.selected_limit = 0
    var result = dc_context_range()
    return dc_context_budget(&caps, &result) == DC_CONTEXT_VALID ? result : nil
  }
  static func choice(_ value: Int, within range: dc_context_range) -> Int {
    min(Int(range.maximum), max(Int(range.minimum), value))
  }
}
