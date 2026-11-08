import Foundation
@main enum ChatPresentationChecks {
  static func main() {
    let code = "print(\"甲 🐦\")\r\n\treturn 1\r\n"
    let doc = MarkdownDocument("Hello **world**\r\n\r\n```swift\r\n" + code + "```\r\nTail")
    assert(doc.blocks == [.paragraph("Hello **world**"), .code(language: "swift", text: code, closed: true), .paragraph("Tail")])
    assert(MarkdownDocument("```python\nprint(1)").blocks == [.code(language: "python", text: "print(1)", closed: false)])
    assert(MarkdownDocument("```\n").blocks == [.code(language: "", text: "", closed: false)])
    assert(MarkdownDocument("~~~~ js extra\n```\n~~~~~\n").blocks == [.code(language: "js", text: "```\n", closed: true)])
    assert(MarkdownDocument("   ```c\n  int x;\n   ```\n").blocks == [.code(language: "c", text: "  int x;\n", closed: true)])
    assert(MarkdownDocument("    ```c\n    code\n    ```").blocks == [.paragraph("    ```c\n    code\n    ```")])
    assert(MarkdownDocument("```bad`info\ntext").blocks == [.paragraph("```bad`info\ntext")])
    assert(MarkdownDocument("````\n```\nbody\n````suffix\n````\n").blocks == [.code(language: "", text: "```\nbody\n````suffix\n", closed: true)])
    assert(MarkdownDocument("# Heading\n\n- One\n2. Two\n> Quote\n> next\n\n---").blocks == [.heading(1,"Heading"),.listItem("-","One"),.listItem("2.","Two"),.quote("Quote\nnext"),.rule])
    assert(MarkdownDocument("| A | B |\n| --- | :---: |\n| `x|y` | a\\|b |\n").blocks == [.table(["A","B"],[["`x|y`","a\\|b"]])])
    assert(MarkdownDocument("<script>alert('source')</script>\n![local alt](https://example.invalid/image)").blocks == [.paragraph("<script>alert('source')</script>\n![local alt](https://example.invalid/image)")])
    assert(MarkdownDocument("").blocks.isEmpty)
    var cursor = ChatStreamCursor()
    let first = cursor.start(after: 4)
    assert(cursor.requested(first))
    let older = cursor.ticket(first, through: 6, settlesModel: true)!
    let newer = cursor.ticket(first, through: 7, settlesModel: false)!
    assert(cursor.apply(newer) == false)
    assert(cursor.apply(older) == nil) // out-of-order WAL completion
    let oldAttempt = cursor.ticket(first, through: 8, settlesModel: true)!
    assert(cursor.requested(first))
    assert(cursor.apply(oldAttempt) == false) // never clear a new attempt's delta
    let current = cursor.ticket(first, through: 10, settlesModel: true)!
    assert(cursor.apply(current) == true)
    assert(cursor.apply(current) == nil) // duplicate cannot settle twice
    assert(cursor.ticket(first, through: -1, settlesModel: true) == nil)
    cursor.finish()
    assert(cursor.apply(current) == nil && !cursor.requested(first))
    let second = cursor.start(after: 0)
    assert(cursor.ticket(first, through: 100, settlesModel: true) == nil)
    assert(cursor.requested(second))
    assert(cursor.apply(cursor.ticket(second, through: 1, settlesModel: true)!) == true)
    print("PASS exact CRLF/Unicode code copy, complete/partial/nested/invalid fences, ordinary block formatting, inert raw HTML/links; stale/out-of-order/duplicate/next-attempt/finished-run stream snapshots")
  }
}
