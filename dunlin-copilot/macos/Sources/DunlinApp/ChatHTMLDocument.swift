import Foundation
import CryptoKit

// Only host templates become markup. Original model text remains the authority
// for history, indexing, copying and subsequent requests.
struct ChatHTMLDocument {
  let html: String
  let code: [String]
  let sourceID: String
  init(_ source: String, dark: Bool, fontSize: Double, palette:[String:String] = [:]) {
    let sourceID = SHA256.hash(data: Data(source.utf8)).map { String(format: "%02x", $0) }.joined()
    self.sourceID = sourceID
    var parts: [String] = [], code: [String] = []
    for block in MarkdownDocument(source).blocks {
      switch block {
      case .paragraph(let text): parts.append("<p>\(Self.inline(text))</p>")
      case .heading(let level, let text): parts.append("<h\(level)>\(Self.inline(text))</h\(level)>")
      case .quote(let text): parts.append("<blockquote>\(Self.inline(text))</blockquote>")
      case .listItem(let marker, let text):
        let label = ["-", "+", "*"].contains(marker) ? "•" : marker
        parts.append("<div class='list-item'><span class='marker'>\(Self.escape(label))</span><div>\(Self.inline(text))</div></div>")
      case .rule: parts.append("<hr>")
      case .table(let header, let rows):
        let headings = header.map { "<th>\(Self.inline($0))</th>" }.joined()
        let body = rows.map { "<tr>" + $0.map { "<td>\(Self.inline($0))</td>" }.joined() + "</tr>" }.joined()
        parts.append("<div class='table-scroll'><table><thead><tr>\(headings)</tr></thead><tbody>\(body)</tbody></table></div>")
      case .code(let language, let text, let closed):
        let index = code.count; code.append(text)
        parts.append("<section class='code-block'><header><span>\(Self.escape(language.isEmpty ? "代码" : language))\(closed ? "" : " · 生成中")</span><a href='dunlin-copy:\(sourceID)/\(index)'>复制代码</a></header><pre><code>\(Self.escape(text))</code></pre></section>")
      }
    }
    self.code = code
    let size = min(32, max(12, fontSize.isFinite ? fontSize : 16))
    func color(_ key:String,_ fallback:String)->String {
      guard let value=palette[key],value.utf8.count==7,value.first=="#",
        value.dropFirst().utf8.allSatisfy({(48...57).contains($0)||(65...70).contains($0)||(97...102).contains($0)}) else{return fallback}
      return value
    }
    let text=color("text",dark ? "#eceef2":"#202328")
    let muted=color("secondary",dark ? "#a9adb7":"#747984")
    let surface=color("surface",dark ? "#2A2E35":"#FFFFFF")
    let link=color("accent",dark ? "#78b5ff":"#1769c2")
    html = """
    <!doctype html><html lang="zh-CN"><head><meta charset="utf-8">
    <meta name="viewport" content="width=device-width,initial-scale=1">
    <meta http-equiv="Content-Security-Policy" content="default-src 'none'; style-src 'unsafe-inline'; script-src 'none'; img-src 'none'; connect-src 'none'; font-src 'none'; media-src 'none'; frame-src 'none'; object-src 'none'; base-uri 'none'; form-action 'none'">
    <style>
    :root{color-scheme:\(dark ? "dark" : "light");--text:\(text);--muted:\(muted);--line:\(dark ? "#414650" : "#e3e6ec");--code:\(dark ? "#22252b" : "#f4f6f9");--link:\(link)}
    *{box-sizing:border-box}html,body{margin:0;padding:0;background:\(surface)}body{font-family:-apple-system,BlinkMacSystemFont,sans-serif;font-size:\(size)px;color:var(--text);line-height:1.7;overflow-wrap:anywhere;-webkit-user-select:text}article>:first-child{margin-top:0}article>:last-child{margin-bottom:0}p{margin:0 0 16px;white-space:pre-wrap}h1,h2,h3,h4,h5,h6{line-height:1.4;font-weight:650;margin:22px 0 12px}h1{font-size:1.4em}h2{font-size:1.22em}h3{font-size:1.1em}h4,h5,h6{font-size:1em}a{color:var(--link);text-decoration:none}a:hover{text-decoration:underline}blockquote{margin:14px 0;padding:4px 14px;border-left:3px solid var(--line);color:var(--muted);white-space:pre-wrap}.list-item{display:flex;gap:10px;margin:5px 0}.marker{flex:none;min-width:20px;color:var(--muted)}.list-item>div{min-width:0;white-space:pre-wrap}hr{border:0;border-top:1px solid var(--line);margin:20px 0}code{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-size:.88em}p code,td code,li code{background:var(--code);padding:2px 5px;border-radius:4px}.code-block{margin:16px 0;border:1px solid var(--line);border-radius:10px;overflow:hidden;background:var(--code)}.code-block header{display:flex;justify-content:space-between;gap:16px;padding:9px 13px;border-bottom:1px solid var(--line);font-size:12px;color:var(--muted)}pre{margin:0;padding:14px;white-space:pre;overflow:auto;max-height:420px;line-height:1.55}.table-scroll{overflow:auto;margin:16px 0;border:1px solid var(--line);border-radius:9px}table{width:100%;border-collapse:collapse;font-size:.92em}th,td{text-align:left;vertical-align:top;padding:10px 14px;min-width:110px;border-bottom:1px solid var(--line)}th{font-weight:600;background:var(--code)}tbody tr:last-child td{border-bottom:0}strong{font-weight:650}
    </style></head><body><article>\(parts.joined())</article></body></html>
    """
  }
  static func escape(_ text: String) -> String {
    text.replacingOccurrences(of: "&", with: "&amp;").replacingOccurrences(of: "<", with: "&lt;")
      .replacingOccurrences(of: ">", with: "&gt;").replacingOccurrences(of: "\"", with: "&quot;")
      .replacingOccurrences(of: "'", with: "&#39;")
  }
  static func externalLink(_ text: String) -> URL? {
    guard let url = URL(string: text), let scheme = url.scheme?.lowercased(),
      ["https", "http"].contains(scheme), url.host?.isEmpty == false,
      url.user == nil, url.password == nil else { return nil }
    return url
  }
  private static func inline(_ text: String) -> String {
    guard let attributed = try? AttributedString(markdown: text,
      options: .init(interpretedSyntax: .inlineOnlyPreservingWhitespace)) else { return escape(text) }
    return attributed.runs.map { run in
      var content = escape(String(attributed.characters[run.range]))
      let intent = run.inlinePresentationIntent ?? []
      if intent.contains(.code) { content = "<code>\(content)</code>" }
      if intent.contains(.stronglyEmphasized) { content = "<strong>\(content)</strong>" }
      if intent.contains(.emphasized) { content = "<em>\(content)</em>" }
      if intent.contains(.strikethrough) { content = "<s>\(content)</s>" }
      if let link = run.link, externalLink(link.absoluteString) != nil {
        content = "<a href='\(escape(link.absoluteString))' rel='noreferrer noopener'>\(content)</a>"
      }
      return content
    }.joined()
  }
}
