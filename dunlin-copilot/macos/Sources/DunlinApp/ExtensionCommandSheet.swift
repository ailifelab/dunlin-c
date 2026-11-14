import SwiftUI
indirect enum JSONValue:Codable {
  case object([String:JSONValue]),array([JSONValue]),string(String),integer(Int64),number(Double),bool(Bool),null
  init(from decoder:Decoder)throws{
    let c=try decoder.singleValueContainer()
    if c.decodeNil(){self = .null}
    else if let v=try? c.decode(Bool.self){self = .bool(v)}
    else if let v=try? c.decode(Int64.self){self = .integer(v)}
    else if let v=try? c.decode(Double.self){self = .number(v)}
    else if let v=try? c.decode(String.self){self = .string(v)}
    else if let v=try? c.decode([String:JSONValue].self){self = .object(v)}
    else{self = .array(try c.decode([JSONValue].self))}
  }
  func encode(to encoder:Encoder)throws{var c=encoder.singleValueContainer();switch self{case .object(let v):try c.encode(v);case .array(let v):try c.encode(v);case .string(let v):try c.encode(v);case .integer(let v):try c.encode(v);case .number(let v):try c.encode(v);case .bool(let v):try c.encode(v);case .null:try c.encodeNil()}}
  var formatted:String{let encoder=JSONEncoder();encoder.outputFormatting=[.prettyPrinted,.sortedKeys,.withoutEscapingSlashes];return (try? encoder.encode(self)).flatMap{String(data:$0,encoding:.utf8)} ?? ""}
}
struct ExtensionCommandSelection:Identifiable {
  var id:String{source+":"+package+":"+command}
  let source:String;let package:String;let command:String;let title:String
  var version:String?=nil;var instance:String?=nil
}
struct ExtensionCommandSheet:View {
  @EnvironmentObject var store:WorkspaceStore
  @Environment(\.dismiss) private var dismiss
  let selection:ExtensionCommandSelection
  @State private var arguments="{}";@State private var commandInput="";@State private var result="";@State private var error="";@State private var busy=false
  @State private var cancel:Callbacks?
  var body:some View {
    VStack(alignment:.leading,spacing:18){
      HStack{Text(selection.title).font(.title2.weight(.semibold));Spacer();Button("完成"){dismiss()}.disabled(busy)}
      Text("当前聊天：\(store.selectedChat?.title ?? "未选择")").font(.callout).foregroundStyle(.secondary)
      if selection.command.hasPrefix("slash:") {
        Text("命令参数").font(.headline)
        TextEditor(text:$commandInput).font(.body).frame(height:110).padding(8).background(.quaternary.opacity(0.3),in:RoundedRectangle(cornerRadius:10)).disabled(busy).accessibilityLabel("命令参数")
        Text(selection.command=="slash:checkpoint" ? "例如：note 修改前的记录" : selection.command=="slash:rewind" ? "例如：preview 检查点编号，或 session 检查点编号" : "填写命令需要的参数。").font(.caption).foregroundStyle(.secondary)
      } else {
        Text("参数（JSON 对象）").font(.headline)
        TextEditor(text:$arguments).font(.system(.body, design: .monospaced)).frame(height:140).border(.quaternary).disabled(busy)
      }
      HStack{Text("命令：\(selection.command.hasPrefix("slash:") ? "/"+String(selection.command.dropFirst(6)):selection.command)").font(.caption).foregroundStyle(.secondary);Spacer();if busy{ProgressView().controlSize(.small);Button("停止"){cancel?.cancel()}}else{Button("执行"){run()}.buttonStyle(.borderedProminent)}}
      if !error.isEmpty{Text(error).foregroundStyle(.red)}
      ScrollView{Text(result).font(.system(.body, design: .monospaced)).textSelection(.enabled).frame(maxWidth:.infinity,alignment:.leading)}
    }.padding(28).frame(width:720,height:600).onDisappear{cancel?.cancel()}
      .onAppear{if selection.command.hasPrefix("slash:"){arguments="{\"input\": \"\"}"}}
      .onChange(of:store.identityEpoch){_,_ in cancel?.cancel();dismiss()}
  }
  private func readableResult(_ value:JSONValue?)->String {
    guard let value else{return ""};var current=value
    if case .object(let fields)=current,let nested=fields["data"] {current=nested}
    if selection.command.hasPrefix("slash:"),case .object(let fields)=current,case .object(let outcome)?=fields["result"],case .string(let text)?=outcome["text"] {return text}
    return value.formatted
  }
  private func run(){
    let object:[String:Any]
    if selection.command.hasPrefix("slash:") {
      var input=commandInput;let prefix="/"+String(selection.command.dropFirst(6))
      if input==prefix {input=""}else if input.hasPrefix(prefix+" "){input=String(input.dropFirst(prefix.count+1))}
      guard input.utf8.count<=16_384 else{error="命令参数过长，最大 16 KiB。";return};object=["input":input]
    } else {
      guard arguments.utf8.count<=131072,let data=arguments.data(using:.utf8),let parsed=(try? JSONSerialization.jsonObject(with:data)) as? [String:Any] else{error="请输入有效 JSON 对象，最大 128 KiB。";return};object=parsed
    }
    busy=true;error="";result="";let box=Callbacks();cancel=box
    var request:[String:Any]=["op":"extensions.command","source":selection.source,"package":selection.package,"command":selection.command,"arguments":object]
    if let chat=store.selected {request["chat"]=chat}
    if let version=selection.version {request["version"]=version};if let instance=selection.instance {request["instance"]=instance}
    store.extensionRequest(request,as:JSONValue.self,cancel:box){r in
      busy=false;cancel=nil;result=readableResult(r.data);error=r.ok ? "":extensionReason(r.error ?? "扩展命令未完成");store.refreshThemes();store.reload();store.reloadProjects()
    }
  }
}
