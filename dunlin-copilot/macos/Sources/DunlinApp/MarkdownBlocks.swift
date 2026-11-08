import Foundation

// Small presentation parser; the original message remains the copy/search source.
// Fenced bodies retain their exact UTF-8 text and line endings, including during
// streaming. No HTML, script, image loading or filesystem operation is performed.
struct MarkdownDocument {
  enum Block: Equatable {
    case paragraph(String)
    case heading(Int, String)
    case quote(String)
    case listItem(String, String)
    case rule
    case table([String], [[String]])
    case code(language: String, text: String, closed: Bool)
  }
  private struct Line {
    let text: String
    let raw: String
  }
  let blocks: [Block]
  init(_ source: String) {
    let bytes = Array(source.utf8)
    var lines: [Line] = []
    var start = 0
    for i in bytes.indices where bytes[i] == 10 {
      let end = i > start && bytes[i - 1] == 13 ? i - 1 : i
      lines.append(Line(text: String(decoding: bytes[start..<end], as: UTF8.self),
                        raw: String(decoding: bytes[start...i], as: UTF8.self)))
      start = i + 1
    }
    if start < bytes.count {
      let last = String(decoding: bytes[start...], as: UTF8.self)
      lines.append(Line(text: last, raw: last))
    }
    var output: [Block] = []
    var prose: [Line] = []
    var i = 0
    func flush() { output.append(contentsOf: Self.prose(prose.map(\.text))); prose = [] }
    while i < lines.count {
      guard let fence = Self.opening(lines[i].text) else { prose.append(lines[i]); i += 1; continue }
      flush()
      i += 1
      var body = ""
      while i < lines.count && !Self.closing(lines[i].text, fence: fence) {
        body += lines[i].raw
        i += 1
      }
      let closed = i < lines.count
      if closed { i += 1 }
      output.append(.code(language: fence.language, text: body, closed: closed))
    }
    flush()
    blocks = output
  }
  private struct Fence { let char: UInt8; let count: Int; let language: String }
  private static func indentation(_ line: String) -> ArraySlice<UInt8>? {
    let bytes = Array(line.utf8)
    var i = 0
    while i < bytes.count && bytes[i] == 32 { i += 1 }
    return i <= 3 ? bytes[i...] : nil
  }
  private static func opening(_ line: String) -> Fence? {
    guard let bytes = indentation(line), let first = bytes.first, first == 96 || first == 126 else { return nil }
    let count = bytes.prefix(while: { $0 == first }).count
    guard count >= 3 else { return nil }
    let tail = bytes.dropFirst(count)
    guard first != 96 || !tail.contains(96) else { return nil }
    let info = String(decoding: tail, as: UTF8.self).trimmingCharacters(in: .whitespaces)
    let language = String(info.prefix(while: { !$0.isWhitespace && !$0.isNewline }))
    return Fence(char: first, count: count, language: language)
  }
  private static func closing(_ line: String, fence: Fence) -> Bool {
    guard let bytes = indentation(line) else { return false }
    let count = bytes.prefix(while: { $0 == fence.char }).count
    return count >= fence.count && bytes.dropFirst(count).allSatisfy { $0 == 32 || $0 == 9 }
  }
  private static func list(_ line: String) -> (String, String)? {
    guard let bytes = indentation(line) else { return nil }
    let text = String(decoding: bytes, as: UTF8.self)
    if let first = text.first, ["-", "+", "*"].contains(first), text.dropFirst().first == " " {
      return (String(first), String(text.dropFirst(2)))
    }
    let digits = text.prefix(while: { $0.isASCII && $0.isNumber })
    guard !digits.isEmpty, digits.count <= 9 else { return nil }
    let rest = text.dropFirst(digits.count)
    guard let marker = rest.first, marker == "." || marker == ")", rest.dropFirst().first == " " else { return nil }
    return (String(digits) + String(marker), String(rest.dropFirst(2)))
  }
  // Recognize only ordinary pipe tables. Escaped pipes and inline code remain
  // inside cells; ambiguous/unbalanced code is left as ordinary source text.
  private static func cells(_ line: String) -> [String]? {
    var cells: [String] = [], current = "", escaped = false, code = 0, run = 0
    let chars = Array(line.trimmingCharacters(in: .whitespaces))
    var i = 0
    while i < chars.count {
      let c = chars[i]
      if escaped { current.append(c); escaped = false; i += 1; continue }
      if c == "\\" { current.append(c); escaped = true; i += 1; continue }
      if c == "`" {
        run = 1
        while i + run < chars.count && chars[i + run] == "`" { run += 1 }
        if code == 0 { code = run } else if code == run { code = 0 }
        current += String(repeating: "`", count: run); i += run; continue
      }
      if c == "|" && code == 0 { cells.append(current.trimmingCharacters(in: .whitespaces)); current = "" }
      else { current.append(c) }
      i += 1
    }
    guard code == 0, !cells.isEmpty else { return nil }
    cells.append(current.trimmingCharacters(in: .whitespaces))
    if chars.first == "|" { cells.removeFirst() }
    if chars.last == "|", cells.last == "" { cells.removeLast() }
    return cells.isEmpty ? nil : cells
  }
  private static func separator(_ cell: String) -> Bool {
    var s = cell[...]
    if s.first == ":" { s = s.dropFirst() }
    if s.last == ":" { s = s.dropLast() }
    return s.count >= 3 && s.allSatisfy { $0 == "-" }
  }
  private static func prose(_ lines: [String]) -> [Block] {
    var blocks: [Block] = [], paragraph: [String] = []
    func flush() {
      if !paragraph.isEmpty { blocks.append(.paragraph(paragraph.joined(separator: "\n"))); paragraph = [] }
    }
    var i = 0
    while i < lines.count {
      let line = lines[i], trimmed = line.trimmingCharacters(in: .whitespaces)
      if trimmed.isEmpty { flush(); i += 1; continue }
      if i + 1 < lines.count, let header = cells(line), let delimiters = cells(lines[i + 1]),
         header.count == delimiters.count, delimiters.allSatisfy(separator) {
        flush(); i += 2; var rows: [[String]] = []
        while i < lines.count, let row = cells(lines[i]), row.count == header.count { rows.append(row); i += 1 }
        blocks.append(.table(header, rows)); continue
      }
      if let bytes = indentation(line) {
        let t = String(decoding: bytes, as: UTF8.self)
        let hashes = t.prefix(while: { $0 == "#" }).count
        if (1...6).contains(hashes), t.dropFirst(hashes).first == " " {
          flush(); blocks.append(.heading(hashes, String(t.dropFirst(hashes + 1)))); i += 1; continue
        }
        if t == "---" || t == "***" || t == "___" { flush(); blocks.append(.rule); i += 1; continue }
        if t == ">" || t.hasPrefix("> ") {
          flush(); var quote: [String] = []
          while i < lines.count, let b = indentation(lines[i]) {
            let q = String(decoding: b, as: UTF8.self)
            guard q == ">" || q.hasPrefix("> ") else { break }
            quote.append(q == ">" ? "" : String(q.dropFirst(2))); i += 1
          }
          blocks.append(.quote(quote.joined(separator: "\n"))); continue
        }
      }
      if let (marker, text) = list(line) { flush(); blocks.append(.listItem(marker, text)); i += 1; continue }
      paragraph.append(line); i += 1
    }
    flush(); return blocks
  }
}
