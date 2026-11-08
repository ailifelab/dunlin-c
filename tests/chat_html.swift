import Foundation

@main struct HTMLChecks {
  static func main() {
    let exact = "let x = \"<&>\"\r\n  保留缩进\r\n"
    let source = "# 清楚的回复\n\n**重点** 与 `code`\n\n- 第一项\n- 第二项\n\n| 项目 | 结果 |\n| --- | --- |\n| 文件 | 已完成 |\n\n> 引用原文\n\n```swift\r\n" + exact + "```\r\n"
    let doc = ChatHTMLDocument(source, dark: false, fontSize: 16)
    precondition(doc.code == [exact])
    for value in ["<h1>清楚的回复</h1>", "<strong>重点</strong>", "<code>code</code>",
      "<table>", "<th>项目</th>", "<td>已完成</td>", "<blockquote>", "class='list-item'",
      "href='dunlin-copy:\(doc.sourceID)/0'", "&lt;&amp;&gt;"] { precondition(doc.html.contains(value), value) }
    let hostile = "[x](javascript:alert(1)) [local](file:///private/fixture) [web](https://example.com/test)\n\n<script>alert('x')</script><img src='http://127.0.0.1:1/pixel'><iframe src='file:///private/fixture'></iframe>"
    let safe = ChatHTMLDocument(hostile, dark: true, fontSize: .infinity).html
    precondition(!safe.contains("<script>") && !safe.contains("<img ") && !safe.contains("<iframe "))
    precondition(!safe.contains("href='javascript:") && !safe.contains("href='file:"))
    precondition(safe.contains("href='https://example.com/test'"))
    precondition(safe.contains("script-src 'none'") && safe.contains("connect-src 'none'") && safe.contains("img-src 'none'"))
    precondition(safe.contains("color-scheme:dark") && safe.contains("font-size:16.0px"))
    for bad in ["javascript:alert(1)","file:///tmp/x","data:text/html,abc","dunlin-copy:0","https://user:secret@example.com","https:///", "mailto:a@example.com"] {
      precondition(ChatHTMLDocument.externalLink(bad) == nil, bad)
    }
    precondition(ChatHTMLDocument.externalLink("https://example.com/a?q=x") != nil)
    precondition(ChatHTMLDocument("```\nunfinished", dark: false, fontSize: 10).code == ["unfinished"])
    precondition(ChatHTMLDocument("", dark: false, fontSize: 16).code.isEmpty)
    let themed=ChatHTMLDocument(source,dark:false,fontSize:16,palette:["text":"#123ABC","secondary":"#456789","accent":"#00aaff","surface":"#182D3D"])
    precondition(themed.html.contains("--text:#123ABC;--muted:#456789") && themed.html.contains("--link:#00aaff"))
    precondition(themed.sourceID==doc.sourceID && themed.code==doc.code)
    let injected=ChatHTMLDocument(source,dark:false,fontSize:16,palette:["text":"#123456;}</style><script>evil</script>","accent":"#12zzzz"])
    precondition(!injected.html.contains("<script>evil") && injected.html.contains("--text:#202328") && injected.html.contains("--link:#1769c2"))
    print("PASS strict third-party color projection, rejected CSS injection and preserved source/code identity")
    print("PASS HTML layout contract: CJK headings/lists/tables/emphasis, exact CRLF code copy source, escaping, CSP and explicit external link policy")
  }
}
