import CDesktop

@main struct BudgetChecks {
  static func main() {
    var caps = dc_context_capabilities(model_limit: 0, deployment_limit: 8192,
      selected_limit: 100000, client_limit: 4_194_304, output_limit: 65536,
      requested_output: 1024, overhead_reserve: 512)
    var strict = dc_context_range()
    precondition(dc_context_budget(&caps, &strict) == DC_CONTEXT_INVALID)
    let bounds = ModelContextBudget.preview(caps, choice: nil)!
    precondition(bounds.minimum == 1537 && bounds.maximum == 8192)
    precondition(bounds.recommended == 8192 && bounds.input_budget == 6656)
    for (input, expected) in [(Int.min, 1537), (-1, 1537), (0, 1537), (1536, 1537),
      (1537, 1537), (5000, 5000), (8192, 8192), (8193, 8192), (100000, 8192), (Int.max, 8192)] {
      let choice = Int(ModelContextBudget.preview(caps, choice: input)!.selected)
      precondition(choice == expected)
      var candidate = caps
      candidate.selected_limit = UInt64(choice)
      precondition(dc_context_budget(&candidate, &strict) == DC_CONTEXT_VALID)
      precondition(strict.input_budget + strict.output + candidate.overhead_reserve == UInt64(choice))
    }
    // Editing does not relax C's strict persisted/send contract or mutate capabilities.
    precondition(caps.selected_limit == 100000)
    precondition(dc_context_budget(&caps, &strict) == DC_CONTEXT_INVALID)
    caps.model_limit = 4096
    precondition(ModelContextBudget.preview(caps, choice: nil)!.maximum == 4096)
    caps.client_limit = 2048
    precondition(ModelContextBudget.preview(caps, choice: nil)!.maximum == 2048)
    caps.requested_output = 5000
    precondition(ModelContextBudget.preview(caps, choice: nil) == nil)
    caps.output_limit = 1024
    precondition(ModelContextBudget.preview(caps, choice: nil)!.minimum == 1537)
    caps.overhead_reserve = 1023
    let single = ModelContextBudget.preview(caps, choice: nil)!
    precondition(single.minimum == 2048 && single.maximum == 2048 && single.input_budget == 1)
    caps.overhead_reserve = 1024
    precondition(ModelContextBudget.preview(caps, choice: nil) == nil)
    caps.model_limit = 0; caps.deployment_limit = 0
    precondition(ModelContextBudget.preview(caps, choice: nil) == nil)
    caps.deployment_limit = 65536; caps.client_limit = 4_194_304
    caps.overhead_reserve = 512; caps.requested_output = 0
    precondition(ModelContextBudget.preview(caps, choice: nil) == nil)
    caps.requested_output = 1024
    precondition(ModelContextBudget.preview(caps, choice: nil)!.recommended == 32768)
    print("PASS: bounded native editor choices, C strict validation, unknown and impossible capabilities")
  }
}
