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
}
struct ExtensionCommandSheet:View {
  @EnvironmentObject var store:WorkspaceStore
  @Environment(\.dismiss) private var dismiss
  let selection:ExtensionCommandSelection
  @State private var arguments="{}";@State private var result="";@State private var error="";@State private var busy=false
  @State private var cancel:Callbacks?
  var body:some View {
    VStack(alignment:.leading,spacing:18){
      HStack{Text(selection.title).font(.title2.weight(.semibold));Spacer();Button("完成"){dismiss()}.disabled(busy)}
      Text("当前聊天：\(store.selectedChat?.title ?? "未选择")").font(.callout).foregroundStyle(.secondary)
      Text("参数（JSON 对象）").font(.headline)
      TextEditor(text:$arguments).font(.system(size:13,design:.monospaced)).frame(height:140).border(.quaternary).disabled(busy)
      HStack{Text("命令：\(selection.command)").font(.caption).foregroundStyle(.secondary);Spacer();if busy{ProgressView().controlSize(.small);Button("停止"){cancel?.cancel()}}else{Button("执行"){run()}.buttonStyle(.borderedProminent)}}
      if !error.isEmpty{Text(error).foregroundStyle(.red)}
      ScrollView{Text(result).font(.system(size:13,design:.monospaced)).textSelection(.enabled).frame(maxWidth:.infinity,alignment:.leading)}
    }.padding(28).frame(width:720,height:600).onDisappear{cancel?.cancel()}
      .onAppear{if selection.command.hasPrefix("slash:"){arguments="{\"input\": \"\"}"}}
      .onChange(of:store.identityEpoch){_,_ in cancel?.cancel();dismiss()}
  }
  private func run(){
    guard arguments.utf8.count<=131072,let data=arguments.data(using:.utf8),let object=(try? JSONSerialization.jsonObject(with:data)) as? [String:Any] else{error="请输入有效 JSON 对象，最大 128 KiB。";return}
    busy=true;error="";result="";let box=Callbacks();cancel=box
    var request:[String:Any]=["op":"extensions.command","source":selection.source,"package":selection.package,"command":selection.command,"arguments":object]
    if let chat=store.selected {request["chat"]=chat}
    store.extensionRequest(request,as:JSONValue.self,cancel:box){r in
      busy=false;cancel=nil;result=r.data?.formatted ?? "";error=r.ok ? "":extensionReason(r.error ?? "扩展命令未完成");store.refreshThemes()
    }
  }
}
