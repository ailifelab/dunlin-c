import CDesktop
import AppKit
import SwiftUI

struct KnowledgeBody:Decodable {
  let text:String?;let subject:String?;let kind:String?;let status:String?;let speaker:String?;let speakerType:String?;let sourceRole:String?;let sourceQuote:String?;let quote:String?;let expiresAt:Int64?;let sha256:String?;let document:String?;let start:Int?;let end:Int?
}
struct KnowledgeNode:Decodable,Identifiable {
  let id:String;let sequence:Int64;let owner:String;let kind:String;let scope:String;let scopeID:String;let name:String;let body:KnowledgeBody;let revision:Int64;let state:String;let sourceChat:String;let sourceSequence:Int;let sourceHash:String;let independent:Bool;let created:Int64;let updated:Int64
}
struct KnowledgePage:Decodable {let rows:[KnowledgeNode];let next:Int64;let hasMore:Bool}
struct MemorySubject:Decodable,Identifiable {let id:String;let name:String;let kind:String}
struct MemorySelection:Identifiable {
  var id=UUID();var chat="";var sequence = -1;var sourceHash="";var text="";var role="user"
}
extension WorkspaceStore {
  func knowledgeRequest<T:Decodable>(_ body:[String:Any],as:T.Type,completion:@escaping(WorkspaceReply<T>)->Void){
    guard let handle,let data=try? JSONSerialization.data(withJSONObject:body),let raw=String(data:data,encoding:.utf8) else{return}
    let epoch=identityEpoch
    queue.async{let result=self.decode(dc_knowledge_call(handle.raw,raw),as:WorkspaceReply<T>.self) ?? WorkspaceReply(ok:false,error:"知识服务未返回有效结果",data:nil);DispatchQueue.main.async{if self.identityEpoch==epoch{completion(result)}}}
  }
  func registerMemorySubjects(){
    for identity in identities where !identity.hidden {knowledgeRequest(["op":"memory.subject.add","id":identity.id,"name":identity.name,"kind":identity.kind],as:NoData.self){_ in}}
  }
  func remember(_ message:Message){guard let chat=selected,let sequence=message.sequence,let hash=message.sourceHash else{error="这条消息缺少稳定来源信息，请重新载入聊天。";return};remembering=MemorySelection(chat:chat,sequence:sequence,sourceHash:hash,text:message.content,role:message.role)}
}
enum KnowledgePageRoute {
  case overview, memory, documents
  var title:String {
    switch self {
    case .overview: "记忆与知识库"
    case .memory: "人物记忆"
    case .documents: "知识库"
    }
  }
}

struct KnowledgeView: View {
  @EnvironmentObject var store: WorkspaceStore
  @State private var choosingShared: Bool?

  var body: some View {
    if store.knowledgePage == .overview {
      VStack(alignment: .leading, spacing: 26) {
        card(minHeight:163) {
          HStack(alignment: .firstTextBaseline) {
            Text("个性化记忆").font(.headline)
            Spacer()
            Toggle("个性化记忆", isOn: $store.knowledgePreferences.memory)
              .labelsHidden().toggleStyle(.switch).accessibilityIdentifier("memory.enabled")
          }
          Text("从已提交的聊天与明确反馈中提取候选，保留来源与有效期。")
          Text("当前指令优先；从他人转述得出的印象不会作为本人确认事实。")
        }
        card(minHeight:155) {
          Text("知识库").font(.headline)
          Text("项目文档、说明书和明确加入的资料可以被检索并引用。")
          HStack {
            Text("私人、家庭和项目资料分别设置可见范围。")
            Spacer()
            Button { store.knowledgePage = .documents } label: {
              Text("管理知识库").frame(minWidth:140)
            }.buttonStyle(.bordered)
              .accessibilityIdentifier("knowledge.manage")
          }
        }
        card(minHeight:142) {
          Text("纠正与忘记").font(.headline)
          Text("查看来源、修改偏好、删除记忆或暂停使用。")
          Text("删除来源聊天会清理依赖它的记忆；独立保存的记忆可以单独删除。")
        }
        HStack(spacing: 24) {
          Button("查看我的记忆 →") { openMemory(shared:false) }.accessibilityIdentifier("memory.personal")
          Button("查看家庭共享记忆 →") { openMemory(shared:true) }.accessibilityIdentifier("memory.household")
        }.buttonStyle(.link).foregroundStyle(store.themeTint).padding(.top,26)
      }.padding(.top,18).sheet(isPresented: Binding(get:{choosingShared != nil},set:{if !$0 {choosingShared=nil}})) {
        let shared = choosingShared == true
        let choices = eligible(shared:shared)
        VStack(alignment:.leading,spacing:18) {
          Text(shared ? "选择家庭共享身份" : "选择个人身份").font(.title2)
          if choices.isEmpty {
            Text("当前身份尚未关联可用的家庭空间。").foregroundStyle(.secondary)
            Button("管理身份与家庭") { choosingShared=nil;store.settingsSection="身份与家庭" }
          } else {
            ForEach(choices) { identity in
              Button(identity.name) { choosingShared=nil;store.selectSettledIdentity(identity,openMemory:true) }
            }
          }
          HStack{Spacer();Button("取消"){choosingShared=nil}}
        }.padding(24).frame(minWidth:360)
      }
    } else {
      KnowledgeLibrary(isMemory:store.knowledgePage == .memory).id(store.knowledgePage)
    }
  }

  private func card<Content:View>(minHeight:CGFloat,@ViewBuilder content:()->Content)->some View {
    VStack(alignment:.leading,spacing:10,content:content)
      .padding(22).frame(maxWidth:.infinity,minHeight:minHeight,alignment:.topLeading)
      .background(store.themeSurface,in:RoundedRectangle(cornerRadius:10))
  }

  private func eligible(shared:Bool)->[DesktopIdentity] {
    guard let current=store.currentIdentity else{return []}
    return store.identities.filter { candidate in
      guard !candidate.hidden else{return false}
      if shared{return candidate.kind=="shared" && candidate.members.contains(current.id)}
      return candidate.kind=="person" && current.members.contains(candidate.id)
    }
  }

  private func openMemory(shared:Bool) {
    guard let current=store.currentIdentity else{return}
    if (current.kind=="shared") == shared {store.knowledgePage = .memory;return}
    let candidates=eligible(shared:shared)
    if candidates.count==1 {store.selectSettledIdentity(candidates[0],openMemory:true)}
    else {choosingShared=shared}
  }
}

private struct KnowledgeLibrary:View {
  let isMemory:Bool
  private var mode:String {isMemory ? "memory":"knowledge"}
  @EnvironmentObject var store:WorkspaceStore
  @State private var scopeChoice="global"
  @State private var rows:[KnowledgeNode]=[];@State private var subjects:[MemorySubject]=[]
  @State private var query="";@State private var hits:[KnowledgeNode]=[];@State private var error=""
  @State private var next:Int64=0;@State private var hasMore=false
  @State private var newMemory=false;@State private var editing:KnowledgeNode?;@State private var document=false;@State private var details:KnowledgeNode?
  var scope:String{scopeChoice.hasPrefix("project:") ? "project":scopeChoice.hasPrefix("chat:") ? "chat":"global"}
  var scopeID:String{scopeChoice=="global" ? "":String(scopeChoice.dropFirst(scopeChoice.hasPrefix("project:") ? 8:5))}
  var body:some View {
    VStack(alignment:.leading,spacing:22){
      HStack {
        Text((store.currentIdentity?.name ?? "当前身份") + (isMemory ? "的记忆":"的资料")).font(.headline)
        Spacer()
        Button(isMemory ? "添加记忆…" : "＋ 添加资料"){if isMemory {newMemory=true}else{document=true}}
          .buttonStyle(.borderedProminent).accessibilityIdentifier(isMemory ? "memory.add":"knowledge.add")
      }
      if isMemory {
      Picker("作用范围",selection:$scopeChoice){Text("当前身份 · 全局").tag("global");ForEach(store.projects){Text("项目 · "+$0.name).tag("project:"+$0.id)};if let chat=store.selectedChat{Text("当前对话 · "+chat.title).tag("chat:"+chat.id)}}
      Text(mode=="memory" ? "候选需确认后才能用于个性化。人物对象、说话人、原始证据与推断类型分别保存。":"本地分块检索保留原文位置与内容摘要。当前支持 UTF-8 文本、Markdown 与代码资料，按身份和项目隔离。")
        .font(.callout).foregroundStyle(.secondary)
      }
      if !isMemory {
        ForEach(rows){ item in documentCard(item) }
      }
      if isMemory {
      if let chat=store.selectedChat {
        Toggle("允许从「"+chat.title+"」学习",isOn:Binding(get:{learningEnabled == true},set:setSourcePolicy))
          .disabled(learningEnabled == nil || learningSaving).accessibilityIdentifier("memory.chat.learning")
      }
      HStack {TextField("用自然语言查找已确认记忆和资料",text:$query).textFieldStyle(.roundedBorder).onSubmit(search);Button("查找",action:search)}
      if !query.isEmpty {ForEach(hits){hit in Button{details=hit}label:{VStack(alignment:.leading,spacing:5){Text(hit.name).font(.headline);Text(hit.body.text ?? "").lineLimit(3).font(.callout).foregroundStyle(.secondary)}.frame(maxWidth:.infinity,alignment:.leading)}.buttonStyle(.plain)}}
      }
      if rows.isEmpty {ContentUnavailableView(mode=="memory" ? "还没有记忆":"还没有资料",systemImage:mode=="memory" ? "brain.head.profile":"books.vertical",description:Text(mode=="memory" ? "可以手动添加，也可以从聊天消息菜单保存带出处的候选。":"导入文本后可立即在本地检索，原始文件不会被修改。"))}
      if isMemory { ForEach(rows){item in
        GroupBox {
          VStack(alignment:.leading,spacing:12){
            HStack{Label(item.kind=="memory" ? kind(item.body.kind ?? "fact"):item.name,systemImage:item.kind=="memory" ? "brain.head.profile":"doc.text").font(.headline);Spacer();if item.kind=="memory"{Text(status(item)).font(.caption).foregroundStyle(.secondary)}}
            Text(item.body.text ?? "").font(.callout).lineLimit(item.kind=="document" ? 3:8).textSelection(.enabled)
            if item.kind=="memory" {Text("对象：\(subjects.first{$0.id==item.body.subject}?.name ?? item.body.subject ?? "") · 来源：\(item.sourceChat.isEmpty ? "手动保存":item.body.sourceRole=="user" ? "用户原话":"Agent / 工具记录")").font(.caption).foregroundStyle(.secondary)}
            HStack {
              if item.kind=="memory" {
                if item.body.status != "confirmed"{Button("确认"){change(item,status:"confirmed")}}else{Button("暂停使用"){change(item,status:"proposed")}}
                Button("更正…"){editing=item}
              }
              Button("来源与详情"){details=item};Spacer();Button("删除…",role:.destructive){remove(item)}
            }.font(.callout)
          }.padding(12)
        }
      }
      }
      if hasMore{Button("加载更多"){load(more:true)}}
      if !isMemory {
        Text("聊天检索只覆盖当前身份允许的资料。").foregroundStyle(.secondary)
        DisclosureGroup("检索设置") {
          VStack(alignment:.leading,spacing:10) {
            Toggle("按本次问题检索知识库资料",isOn:$store.knowledgePreferences.rag)
            Picker("检索范围",selection:$scopeChoice) {
              Text("当前身份").tag("global");ForEach(store.projects){Text($0.name).tag("project:"+$0.id)}
            }
            HStack {TextField("查找资料",text:$query).onSubmit(search);Button("查找",action:search)}
            ForEach(hits){hit in Button(hit.name){details=hit}}
          }.padding(10)
        }
      }
      Button("返回记忆与知识库"){store.knowledgePage = .overview}.accessibilityIdentifier("knowledge.back")
      if !error.isEmpty{Text(knowledgeReason(error)).foregroundStyle(.red).textSelection(.enabled)}
    }.onAppear{load();loadSubjects();readSourcePolicy()}.onChange(of:store.selected){_,_ in readSourcePolicy()}.onChange(of:scopeChoice){_,_ in load();hits=[]}
      .onChange(of:store.identityEpoch){_,_ in rows=[];hits=[];subjects=[];scopeChoice="global";editing=nil;details=nil;newMemory=false;document=false;load();loadSubjects()}
      .sheet(isPresented:$newMemory,onDismiss:{load()}){MemoryEditor(scope:scope,scopeID:scopeID)}
      .sheet(item:$editing,onDismiss:{load()}){node in MemoryEditor(node:node)}
      .sheet(isPresented:$document,onDismiss:{load()}){KnowledgeImportView(scope:scope=="chat" ? "global":scope,scopeID:scope=="chat" ? "":scopeID)}
      .sheet(item:$details){node in KnowledgeDetailView(node:node)}
      .sheet(item:$moving){node in KnowledgeScopeSheet(node:node){load();hits=[]}}
  }
  @State private var learningEnabled:Bool?
  @State private var learningSaving=false
  private func readSourcePolicy(){
    learningEnabled=nil
    guard let chat=store.selected, isMemory else{return}
    store.knowledgeRequest(["op":"memory.source.policy","chat":chat],as:[String:Bool].self){r in
      guard store.selected==chat else{return}
      if r.ok { learningEnabled=r.data?["enabled"] }else{error=r.error ?? "读取失败"}
    }
  }
  private func setSourcePolicy(_ enabled:Bool){
    guard let chat=store.selected else{return}
    learningSaving=true
    store.knowledgeRequest(["op":"memory.source.policy","chat":chat,"enabled":enabled],as:[String:Bool].self){r in
      learningSaving=false
      guard store.selected==chat else{return}
      if r.ok {learningEnabled=r.data?["enabled"];hits=[]}else{error=r.error ?? "保存失败"}
    }
  }
  @State private var moving:KnowledgeNode?
  @State private var refreshed:String?
  private func documentCard(_ item:KnowledgeNode)->some View {
    VStack(alignment:.leading,spacing:12) {
      HStack {
        Text(item.name).font(.headline)
        Spacer()
        Text(refreshed == item.id ? "索引已更新":"可检索")
          .font(.caption).frame(minWidth:130).padding(.horizontal,10).padding(.vertical,3)
          .background(store.themeTint.opacity(0.12),in:Capsule())
      }
      Text(item.scope == "project" ? "项目可见 · " + (store.projects.first(where:{$0.id == item.scopeID})?.name ?? item.scopeID)
           : store.currentIdentity?.kind == "shared" ? "家庭共享 · 原文留在本机" : "个人私有 · 原文留在本机")
        .font(.callout).foregroundStyle(.secondary)
      HStack(spacing:16) {
        Button("查看来源"){details=item}.accessibilityIdentifier("knowledge.source."+item.id)
        Button("更改范围"){moving=item}.accessibilityIdentifier("knowledge.scope."+item.id)
        Button("更新索引"){
          store.knowledgeRequest(["op":"knowledge.reindex","id":item.id,"revision":item.revision],as:KnowledgeNode.self){r in
            if r.ok {refreshed=item.id;error="";hits=[]}else{error=r.error ?? "更新失败"}
          }
        }.accessibilityIdentifier("knowledge.reindex."+item.id)
        Button("删除…",role:.destructive){remove(item)}.accessibilityIdentifier("knowledge.remove."+item.id)
      }.buttonStyle(.link).foregroundStyle(store.themeTint)
    }.padding(22).frame(maxWidth:.infinity,minHeight:131,alignment:.topLeading)
      .background(store.themeSurface,in:RoundedRectangle(cornerRadius:10))
  }
  private func load(more:Bool=false){let op=mode=="memory" ? "memory.list":"knowledge.list",selectedScope=scopeChoice;store.knowledgeRequest(["op":op,"scope":isMemory ? scope:"all","scopeID":isMemory ? scopeID:"","after":more ? next:0,"limit":30],as:KnowledgePage.self){r in guard scopeChoice==selectedScope,(mode=="memory" ? "memory.list":"knowledge.list")==op else{return};if let page=r.data,r.ok{rows=more ? rows+page.rows:page.rows;next=page.next;hasMore=page.hasMore;error=""}else{error=r.error ?? "读取失败"}}}
  private func loadSubjects(){store.knowledgeRequest(["op":"memory.subjects"],as:[MemorySubject].self){r in subjects=r.data ?? []}}
  private func search(){let selectedScope=scopeChoice,submittedQuery=query;store.knowledgeRequest(["op":"knowledge.search","scope":scope,"scopeID":scopeID,"query":query],as:[KnowledgeNode].self){r in guard scopeChoice==selectedScope,query==submittedQuery else{return};hits=(r.data ?? []).filter{isMemory ? $0.kind=="memory":$0.kind=="chunk"};error=r.error ?? ""}}
  private func change(_ item:KnowledgeNode,status:String){store.knowledgeRequest(["op":"memory.update","id":item.id,"revision":item.revision,"status":status],as:KnowledgeNode.self){r in if r.ok{load()}else{error=r.error ?? "保存失败"}}}
  private func remove(_ item:KnowledgeNode){let a=NSAlert();a.messageText=item.kind=="memory" ? "删除这条记忆？":"删除这份知识资料？";a.informativeText="将停止用于检索与个性化，来源聊天和用户文件保留。";a.addButton(withTitle:"删除");a.addButton(withTitle:"取消");a.buttons[0].keyEquivalent="";a.window.defaultButtonCell=nil;guard a.runModal() == .alertFirstButtonReturn else{return};store.knowledgeRequest(["op":item.kind=="memory" ? "memory.remove":"knowledge.remove","id":item.id,"revision":item.revision],as:NoData.self){r in if r.ok{load()}else{error=r.error ?? "保存失败"};hits=[]}}
  private func kind(_ s:String)->String {s=="preference" ? "偏好":s=="inference" ? "推断":"事实陈述"}
  private func status(_ item:KnowledgeNode)->String {if let expires=item.body.expiresAt,expires>0,expires<Int64(Date().timeIntervalSince1970){return "已过期"};return item.body.status=="confirmed" ? "已确认":item.body.status=="rejected" ? "已拒绝":"待确认"}
}
struct MemoryEditor:View {
  @EnvironmentObject var store:WorkspaceStore
  @Environment(\.dismiss) private var dismiss
  var selection:MemorySelection?;var node:KnowledgeNode?;var scope="global";var scopeID=""
  @State private var text="";@State private var subject="";@State private var kind="preference";@State private var independent=false
  @State private var expires=false;@State private var date=Date().addingTimeInterval(30*86400)
  @State private var subjects:[MemorySubject]=[];@State private var error="";@State private var busy=false;@State private var id=UUID().uuidString
  var body:some View {
    VStack(alignment:.leading,spacing:20){
      Text(node==nil ? "保存记忆候选":"更正记忆").font(.title2.weight(.semibold))
      Form {
        Picker("描述对象",selection:$subject){ForEach(subjects){Text($0.name).tag($0.id)}}.disabled(node != nil)
        Picker("类型",selection:$kind){Text("偏好").tag("preference");Text("事实陈述").tag("fact");Text("推断").tag("inference")}.disabled(node != nil)
        if node==nil{Toggle("设置有效期",isOn:$expires);if expires{DatePicker("有效期至",selection:$date)}}
      }.formStyle(.grouped).frame(height:node==nil ? 180:110)
      TextEditor(text:$text).font(.body).frame(minHeight:100,maxHeight:180).border(.quaternary)
      if let selection {DisclosureGroup("查看来源原文（\(selection.role)）"){ScrollView{Text(selection.text).font(.callout).textSelection(.enabled)}.frame(maxHeight:120)};Toggle("独立保存，删除来源聊天后仍保留",isOn:$independent)}
      Text("候选不是已确认的人物结论。说话人和描述对象分开记录；确认、暂停和更正都在记忆列表中管理。").font(.caption).foregroundStyle(.secondary)
      if !error.isEmpty{Text(knowledgeReason(error)).foregroundStyle(.red)}
      HStack{Spacer();Button("取消"){dismiss()}.disabled(busy);Button(node==nil ? "保存候选":"保存更正"){save()}.buttonStyle(.borderedProminent).disabled(text.isEmpty||subject.isEmpty||busy)}
    }.padding(28).frame(width:700,height:660).onAppear{
      text=node?.body.text ?? ((selection?.text.utf8.count ?? 0)<=4096 ? selection?.text ?? "":"");subject=node?.body.subject ?? store.currentIdentity?.id ?? "local";kind=node?.body.kind ?? "preference"
      store.knowledgeRequest(["op":"memory.subjects"],as:[MemorySubject].self){r in subjects=r.data ?? [];error=r.error ?? ""}
    }
  }
  private func save(){
    busy=true;var r:[String:Any]
    if let node{r=["op":"memory.update","id":node.id,"revision":node.revision,"status":node.body.status ?? "proposed","text":text]}
    else{r=["op":"memory.create","id":id,"text":text,"kind":kind,"subject":subject,"scope":scope,"scopeID":scopeID,"expiresAt":expires ? Int64(date.timeIntervalSince1970):0,"independent":independent];if let selection{r["sourceChat"]=selection.chat;r["sourceSequence"]=selection.sequence;r["sourceHash"]=selection.sourceHash}}
    store.knowledgeRequest(r,as:KnowledgeNode.self){r in busy=false;if r.ok{dismiss()}else{error=r.error ?? "保存失败"}}
  }
}
struct KnowledgeImportView:View {
  @EnvironmentObject var store:WorkspaceStore;@Environment(\.dismiss) private var dismiss
  var scope:String;var scopeID:String
  @State private var name="";@State private var text="";@State private var error="";@State private var busy=false;@State private var id=UUID().uuidString
  var body:some View {
    VStack(alignment:.leading,spacing:18){Text("导入知识资料").font(.title2.weight(.semibold));TextField("资料名称",text:$name).textFieldStyle(.roundedBorder);HStack{Button("选择 UTF-8 文件…"){choose()};Spacer();Text("\(text.utf8.count) / 262144 bytes").font(.caption).foregroundStyle(.secondary)};TextEditor(text:$text).font(.system(.body, design: .monospaced)).border(.quaternary);Text("保存原始文本与片段位置，不改写来源文件。当前使用本地词项检索，不会生成向量或发送到云端。").font(.caption).foregroundStyle(.secondary);if !error.isEmpty{Text(knowledgeReason(error)).foregroundStyle(.red)};HStack{Spacer();Button("取消"){dismiss()};Button("保存资料"){save()}.buttonStyle(.borderedProminent).disabled(name.isEmpty||text.isEmpty||text.utf8.count>262144||busy)}}.padding(28).frame(width:760,height:620)
  }
  private func choose(){let p=NSOpenPanel();p.canChooseDirectories=false;p.allowsMultipleSelection=false;guard p.runModal() == .OK,let url=p.url else{return};do{let n=(try url.resourceValues(forKeys:[.fileSizeKey])).fileSize ?? Int.max;guard n<=262144 else{error="资料最多 256 KiB，请选择需要的文本片段。";return};text=try String(contentsOf:url,encoding:.utf8);name=url.lastPathComponent}catch{self.error=error.localizedDescription}}
  private func save(){busy=true;store.knowledgeRequest(["op":"knowledge.add","id":id,"name":name,"text":text,"scope":scope,"scopeID":scopeID],as:KnowledgeNode.self){r in busy=false;if r.ok{dismiss()}else{error=r.error ?? "保存失败"}}}
}
struct KnowledgeDetailView: View {
  @EnvironmentObject var store: WorkspaceStore
  @Environment(\.dismiss) private var dismiss
  let node: KnowledgeNode
  @State private var related: [KnowledgeNode] = []
  var body: some View {
    VStack(alignment: .leading, spacing: 18) {
      HStack {
        Text(node.name).font(.title2.weight(.semibold))
        Spacer()
        Button("完成") { dismiss() }
      }
      Text("修订 \(node.revision) · \(node.scope) · \(node.owner)")
        .font(.caption).foregroundStyle(.secondary)
      ScrollView {
        VStack(alignment: .leading, spacing: 20) {
          Text(node.body.text ?? node.body.quote ?? "").textSelection(.enabled)
          if !node.sourceChat.isEmpty {
            GroupBox("聊天来源") {
              VStack(alignment: .leading, spacing: 8) {
                Text("\(node.sourceChat) · 原消息 \(node.sourceSequence)")
                  .font(.caption.monospaced())
                Text(node.body.sourceQuote ?? "引用保留在来源记录中。")
                  .font(.callout).textSelection(.enabled)
                Button("定位原消息") {
                  if store.openChat(node.sourceChat, sequence: node.sourceSequence) { dismiss() }
                }
              }
            }
          }
          ForEach(related) { record in
            GroupBox("来源关系") {
              Text(record.body.quote ?? record.body.text ?? record.id)
                .textSelection(.enabled).frame(maxWidth: .infinity, alignment: .leading)
            }
          }
        }
      }
      Text("SHA-256 · \(node.sourceHash)").font(.caption2.monospaced()).textSelection(.enabled)
    }.padding(28).frame(width: 800, height: 650)
      .onAppear {
        store.knowledgeRequest(["op":"graph.neighbors", "id":node.id,
          "type":node.kind == "memory" ? "derivedFrom":"partOf"], as:[KnowledgeNode].self) {
          related = $0.data ?? []
        }
      }
  }
}
func knowledgeReason(_ code:String)->String {
  switch code {
  case "identity_not_bound":return "当前数据尚未绑定可信身份，请重新打开身份。"
  case "revision_conflict","revision_conflict_or_storage_error":return "内容已变化，请刷新后重新操作。"
  case "memory_source_paused":return "已暂停从这段聊天学习。可在人物记忆页恢复。"
  case "memory_source_changed","memory_source_missing":return "来源已变化或不可用，未保存候选。"
  case "invalid_memory_assertion","invalid_memory_text":return "请检查记忆对象、类型和文本长度（最多 4 KiB）。"
  case "invalid_knowledge_document":return "资料名称或文本无效；仅支持最多 256 KiB 的 UTF-8 文本。"
  case "invalid_knowledge_scope":return "所选项目或聊天已不可用。"
  default:return code
  }
}

private struct KnowledgeScopeSheet: View {
  @EnvironmentObject var store:WorkspaceStore
  @Environment(\.dismiss) var dismiss
  let node:KnowledgeNode
  let didMove:()->Void
  @State private var project=""
  @State private var saving=false
  @State private var error=""
  var body:some View {
    VStack(alignment:.leading,spacing:18) {
      Text("更改资料范围").font(.title2)
      Text(node.name).font(.headline)
      Picker("可见范围",selection:$project){
        Text(store.currentIdentity?.kind == "shared" ? "家庭共享" : "个人私有").tag("")
        ForEach(store.projects){Text("项目 · "+$0.name).tag($0.id)}
      }
      if !error.isEmpty{Text(knowledgeReason(error)).foregroundStyle(.red)}
      HStack {
        Button("取消"){dismiss()}
        Spacer()
        Button("保存范围"){
          saving=true
          store.knowledgeRequest(["op":"knowledge.move","id":node.id,"revision":node.revision,
            "scope":project.isEmpty ? "global":"project","scopeID":project],as:KnowledgeNode.self){r in
            saving=false
            if r.ok{didMove();dismiss()}else{error=r.error ?? "保存失败"}
          }
        }.buttonStyle(.borderedProminent).disabled(project==node.scopeID || saving)
      }
    }.onAppear{project=node.scopeID}.padding(24).frame(width:430)
  }
}
