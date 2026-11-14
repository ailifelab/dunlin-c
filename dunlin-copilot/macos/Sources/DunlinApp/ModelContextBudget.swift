import CDesktop

enum ModelContextBudget {
  static func preview(_ capabilities: dc_context_capabilities, choice: Int?) -> dc_context_range? {
    var caps = capabilities
    var result = dc_context_range()
    let status = dc_context_editor_budget(&caps, choice == nil ? 0 : 1, Int64(choice ?? 0), &result)
    return status == DC_CONTEXT_VALID ? result : nil
  }
}
