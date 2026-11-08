import Foundation

// Main-actor-owned ordering state. WAL reads may finish in any order; a ticket
// belongs to one UI run and the model attempt that produced its commit notice.
struct ChatStreamCursor {
  struct Ticket {
    let run: UUID
    let generation: Int
    let through: Int
    let settlesModel: Bool
  }
  private(set) var run: UUID?
  private(set) var stamp = UUID()
  private(set) var generation = 0
  private(set) var through = -1
  mutating func start(after sequence: Int) -> UUID {
    let id = UUID()
    run = id
    stamp = id
    generation = 0
    through = sequence
    return id
  }
  mutating func finish() { run = nil }
  mutating func requested(_ id: UUID) -> Bool {
    guard run == id else { return false }
    generation += 1
    return true
  }
  func ticket(_ id: UUID, through sequence: Int, settlesModel: Bool) -> Ticket? {
    guard run == id, sequence >= 0 else { return nil }
    return Ticket(run: id, generation: generation, through: sequence, settlesModel: settlesModel)
  }
  // nil rejects an old snapshot; true also settles the currently visible stream.
  mutating func apply(_ ticket: Ticket) -> Bool? {
    guard run == ticket.run, ticket.through > through else { return nil }
    through = ticket.through
    return ticket.settlesModel && ticket.generation == generation
  }
}
