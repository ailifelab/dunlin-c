import SwiftUI
import Combine
import AppKit
import CDesktop

struct AgentJob: Decodable, Identifiable {
  let id, project, chat, parent, model, state, task, outcome: String
  let generation, sequence: Int64
  let workspace, policy:String?
  let native:Bool?
  var statusLabel: String {
    switch state {
    case "queued": return "等待执行"
    case "running": return "正在执行"
    case "waiting": return "等待协作"
    case "cancel_requested": return "正在停止"
    case "succeeded": return "已完成"
    case "cancelled": return "已取消"
    case "interrupted": return "执行中断 · 请核对记录"
    default: return outcome == "unknown" ? "结果待核对" : "未完成"
    }
  }
}
extension WorkspaceStore {
  func reloadAgentJobs(startNext: Bool = false,before:Int64 = 0) {
    let token = UUID();agentRequestID = token
    readLocalView(["op":"agents.list","before":before], as:[AgentJob].self) { result in
      guard self.agentRequestID == token else {return}
      guard result.ok, let rows=result.data else {
        self.agentError=result.error ?? "无法读取协作任务"; self.agentQueueRunning=false; return
      }
      if before == 0 {self.agentJobs=rows} else {let known=Set(self.agentJobs.map(\.id));self.agentJobs.append(contentsOf:rows.filter{!known.contains($0.id)})}
      self.agentHasOlderJobs = rows.count == 100
      if !self.agentWorkers.isEmpty || (self.agentQueueRunning && self.busy) {self.refreshAgentQueue();return}
      if startNext && self.agentQueueRunning && !self.busy {
        self.readLocalView(["op":"agents.next","project":self.agentQueueProject],as:[AgentJob].self) { next in
          guard self.agentQueueRunning, !self.busy else { return }
          guard next.ok, let task=next.data?.first else { self.agentQueueRunning=false;return }
          self.runAgent(task)
        }
      }
    }
  }
  func createAgent(project:String, model:String, task:String, parent:String="", chat:String="") {
    guard !busy, !agentLoading, configurationReady else { return }
    agentLoading=true; agentError=""
    workspaceRequest(["op":"agents.create", "id":UUID().uuidString,
      "project":project,"model":model,"task":task,"parent":parent,"chat":chat,"revision":configRevision],
      as:[AgentJob].self) { result in
        self.agentLoading=false
        if result.ok, let rows=result.data { self.agentJobs=rows; self.reload() }
        else { self.agentError=result.error ?? "无法创建协作任务" }
      }
  }
  func runAgent(_ job:AgentJob) {
    guard !busy, job.state=="queued", profiles.contains(where:{$0.id==job.model}) else {
      agentQueueRunning=false; agentError="任务模型不可用，或当前仍有执行中的任务。"; return
    }
    guard configurationReady,!switchingModel,!configSaving,!checkingConnection,
      let workspace=agentWorkspace(job) else{return}
    agentQueueRunning=false;agentQueueProject=job.project;busy=true;stopping=false;agentError=""
    nativeAgentFailures.remove(job.id)
    launchAgentWorker(job,path:workspace.path,policy:workspace.policy)
  }
  func cancelAgent(_ job:AgentJob) {
    if let worker=agentWorkers[job.id] {worker.cancel();return}
    if activeAgentID==job.id { stop(); return }
    guard !busy else { return }
    workspaceRequest(["op":"agents.cancel","id":job.id],as:[AgentJob].self) { result in
      if result.ok, let rows=result.data { self.agentJobs=rows }
      else { self.agentError=result.error ?? "取消任务失败" }
    }
  }
}

struct AgentCollaborationView:View {
  @EnvironmentObject var store:WorkspaceStore
  @State private var project=""
  @State private var model=""
  @State private var task=""
  @State private var sender=""
  @State private var receiver=""
  @State private var message=""
  @State private var deliveryStatus=""
  @State private var followupChat=""
  @State private var creatingTask=false
  @State private var parent=""
  @State private var sharedMessages:[AgentSharedMessage]=[]
  @State private var members:[AgentMember]=[]
  @State private var inviting=false
  @State private var invitedChat=""
  @State private var inspecting:AgentJob?
  private func reloadMembers() {
    guard !project.isEmpty else {members=[];return}
    let requested=project
    store.readLocalView(["op":"agents.members","project":project],as:[AgentMember].self) { result in
      guard project==requested else{return}
      if result.ok,let values=result.data {members=values}
      else {deliveryStatus=result.error ?? "无法读取协作成员"}
    }
  }
  private func reloadMessages(before:Int64=0) {
    guard !project.isEmpty else { sharedMessages=[];return }
    let requested=project
    store.readLocalView(["op":"agents.messages","project":project,"before":before],as:[AgentSharedMessage].self) { result in
      guard project==requested else{return}
      if result.ok,let messages=result.data {
        if before==0 {sharedMessages=messages}else{sharedMessages.append(contentsOf:messages)}
      } else {deliveryStatus=result.error ?? "无法读取协作消息"}
    }
  }
  private var rows:[AgentJob] { store.agentJobs.filter{project.isEmpty || $0.project==project} }
  private var participants:[Chat] {
    let ids=Set(members.map(\.id)+rows.map(\.chat))
    return store.chats.filter{ids.contains($0.id) && $0.project==project && !$0.archived}
  }
  var body:some View {
    ScrollView {
      VStack(alignment:.leading,spacing:22) {
        HStack {
          VStack(alignment:.leading,spacing:6) {
            Text("项目协作").font(.title.weight(.semibold))
            Text("\(participants.count) 段聊天协作中 · 共享任务与明确发送的消息").foregroundStyle(.secondary)
          }
          Spacer()
          Button("邀请项目内聊天"){inviting=true}.disabled(store.busy || project.isEmpty)
          Button("刷新"){store.reloadAgentJobs();reloadMessages();reloadMembers()}
          Button("添加任务"){parent="";followupChat="";creatingTask=true}.disabled(store.busy || project.isEmpty)
          if store.busy { Button("停止当前任务",role:.destructive){store.stop()} }
          else { Button("运行队列"){store.startAgentQueue(project:project)}
            .buttonStyle(.borderedProminent).disabled(!store.agentJobs.contains{$0.state=="queued"}) }
        }
        HStack {
          Picker("项目",selection:$project) {
            Text("选择项目").tag("")
            ForEach(store.projects){Text($0.name).tag($0.id)}
          }
          Picker("模型",selection:$model) {
            Text("选择模型").tag("")
            ForEach(store.profiles){Text($0.name).tag($0.id)}
          }
        }.disabled(store.busy)
        Text("最多同时运行 2 个任务；同一工作目录的写入任务依次执行，工具操作逐次授权。").font(.caption).foregroundStyle(.secondary)
        if !store.agentError.isEmpty { Text(store.agentError).foregroundStyle(.red).textSelection(.enabled) }
        if rows.isEmpty && members.isEmpty { ContentUnavailableView("还没有协作任务",systemImage:"person.3",description:Text("选择项目和模型，添加第一项任务。")) }
        LazyVGrid(columns:[GridItem(.adaptive(minimum:360),spacing:24)],spacing:24) {
        ForEach(members.filter{member in !rows.contains{$0.chat==member.id}}) { member in
          GroupBox {
            VStack(alignment:.leading,spacing:12) {
              Text(member.title).font(.headline)
              Text("协作会话 · 只共享主动发送的消息").foregroundStyle(.secondary)
              HStack {
                Button("查看聊天"){_ = store.openChat(member.id)}.disabled(store.busy)
                Button("安排任务"){followupChat=member.id;parent="";task="";creatingTask=true}.disabled(store.busy)
              }
            }.padding(20).frame(maxWidth:.infinity,alignment:.leading)
          }
        }
        ForEach(rows.filter{$0.parent.isEmpty}) { job in
          GroupBox {
            VStack(alignment:.leading,spacing:10) {
              HStack {
                Label(job.statusLabel,systemImage:job.state=="succeeded" ? "checkmark.circle":"person.crop.circle")
                Spacer()
                Text(store.profiles.first{$0.id==job.model}?.name ?? "模型不可用").foregroundStyle(.secondary)
              }
              Text(job.task).textSelection(.enabled).frame(maxWidth:.infinity,alignment:.leading)
              HStack {
                Button("继续任务"){followupChat=job.chat;project=job.project;model=job.model;parent="";task="";creatingTask=true}.disabled(store.busy)
                if ["queued","running","waiting"].contains(job.state) {
                  Button("添加子任务"){parent=job.id;followupChat="";task="";model=job.model;creatingTask=true}.disabled(store.busy)
                }
              }.fixedSize(horizontal:false,vertical:true)
              HStack {
                Button("查看聊天与记录"){if store.busy {inspecting=job}else{_ = store.openChat(job.chat)}}
                Spacer(minLength:8)
                if job.state=="queued" { Button("执行"){store.runAgent(job)}.disabled(store.busy) }
                if ["queued","running","waiting","cancel_requested"].contains(job.state) {
                  Button("取消",role:.destructive){store.cancelAgent(job)}.disabled(store.busy && store.activeAgentID != job.id && !store.agentRunningIDs.contains(job.id))
                }
              }
            }.padding(12)
          }
        }
        }
        if store.agentHasOlderJobs,let cursor=store.agentJobs.last?.sequence {Button("加载更早的任务") {store.reloadAgentJobs(before:cursor)}}
        Text("子 Agent").font(.system(size:19,weight:.semibold))
        if rows.allSatisfy({$0.parent.isEmpty}) { Text("尚未创建子任务").foregroundStyle(.secondary) }
        ForEach(rows.filter{!$0.parent.isEmpty}) { job in
          HStack(spacing:20) {
            VStack(alignment:.leading,spacing:6) {
              Text(job.task).font(.body.weight(.medium)).lineLimit(2)
              Text("父任务 · \(job.parent.prefix(8))").font(.caption).foregroundStyle(.secondary)
            }
            Spacer()
            Text(job.statusLabel).font(.caption).padding(.horizontal,12).padding(.vertical,6).background(Color.accentColor.opacity(0.1),in:Capsule())
            Button("查看消息"){if store.busy {inspecting=job}else{_ = store.openChat(job.chat)}}
            if job.state=="queued" { Button("执行"){store.runAgent(job)}.disabled(store.busy) }
            if ["queued","running","waiting"].contains(job.state) { Button("取消",role:.destructive){store.cancelAgent(job)}.disabled(store.busy && store.activeAgentID != job.id && !store.agentRunningIDs.contains(job.id)) }
          }.padding(20).background(Color.secondary.opacity(0.05),in:RoundedRectangle(cornerRadius:9))
        }
        GroupBox("来自协作会话") {
          VStack(alignment:.leading,spacing:12) {
            ForEach(sharedMessages) { item in
              VStack(alignment:.leading,spacing:6) {
                HStack {
                  Text(store.chats.first{$0.id==item.sender}?.title ?? item.sender).fontWeight(.medium)
                  Image(systemName:"arrow.right")
                  Text(store.chats.first{$0.id==item.receiver}?.title ?? item.receiver)
                  Spacer()
                  Text(item.delivered ? "已加入原始聊天" : "等待接收").foregroundStyle(.secondary)
                }.font(.caption)
                Text(item.text).textSelection(.enabled)
              }.frame(maxWidth:.infinity,alignment:.leading).padding(.vertical,8)
              Divider()
            }
            if sharedMessages.count>=100,let last=sharedMessages.last {
              Button("更早的协作消息"){reloadMessages(before:last.id)}
            }
            HStack {
              Picker("来自",selection:$sender){Text("选择任务").tag("");ForEach(participants){Text($0.title).tag($0.id)}}
              Picker("发送给",selection:$receiver){Text("选择任务").tag("");ForEach(participants){Text($0.title).tag($0.id)}}
            }
            TextField("消息作为资料加入接收方下一次任务，不改变权限",text:$message).textFieldStyle(.roundedBorder)
            HStack {
              Text(deliveryStatus).font(.caption).foregroundStyle(.secondary)
              Spacer()
              Button("发送协作消息") {
                store.workspaceRequest(["op":"agents.send","project":project,"sender":sender,"receiver":receiver,"id":UUID().uuidString,"text":message],as:AgentMessageReceipt.self){r in
                  deliveryStatus=r.ok ? "已放入收件箱，等待接收方执行任务。" : r.error ?? "发送失败"
                  if r.ok { message="";reloadMessages() }
                }
              }.disabled((store.busy && store.agentWorkers.isEmpty) || project.isEmpty || sender.isEmpty || receiver.isEmpty || message.isEmpty)
            }
          }.padding(10)
        }
      }.padding(48)
    }.sheet(isPresented:$creatingTask) {
      VStack(alignment:.leading,spacing:20) {
        HStack {Text(parent.isEmpty ? "安排任务" : "添加子 Agent").font(.title2.bold());Spacer();Button("取消"){creatingTask=false}}
        Picker("模型",selection:$model){Text("选择模型").tag("");ForEach(store.profiles){Text($0.name).tag($0.id)}}
        GroupBox(followupChat.isEmpty ? "新任务" : "继续任务") {
          VStack(alignment:.leading,spacing:12) {
            TextEditor(text:$task).font(.body).frame(minHeight:80,maxHeight:120)
              .accessibilityIdentifier("agents.task")
            HStack {
              Text("每个任务拥有独立聊天；文件和命令仍逐次请求授权。").font(.caption).foregroundStyle(.secondary)
              Spacer()
              if !followupChat.isEmpty { Button("改为新任务"){followupChat=""} }
              Button("添加任务") {
                store.createAgent(project:project,model:model,task:task,parent:parent,chat:followupChat)
                creatingTask=false
              }.disabled(store.busy || store.agentLoading || project.isEmpty || model.isEmpty || task.trimmingCharacters(in:.whitespacesAndNewlines).isEmpty)
                .accessibilityIdentifier("agents.create")
            }
          }.padding(10)
        }
      }.padding(24).frame(width:620)
    }.sheet(isPresented:$inviting) {
      VStack(alignment:.leading,spacing:20) {
        Text("邀请项目内聊天").font(.title2.bold())
        Text("加入后可主动发送协作消息，其他聊天不会自动读取完整历史。").foregroundStyle(.secondary)
        if !deliveryStatus.isEmpty {Text(deliveryStatus).foregroundStyle(.secondary)}
        Picker("聊天",selection:$invitedChat) {
          Text("选择聊天").tag("")
          ForEach(store.chats.filter{$0.project==project && !$0.archived}) {Text($0.title).tag($0.id)}
        }
        HStack {Spacer();Button("取消"){inviting=false};Button("加入协作") {
          store.workspaceRequest(["op":"agents.join","project":project,"chat":invitedChat],as:[String:String].self) { result in
            if result.ok {inviting=false;reloadMembers()}
            else {deliveryStatus=result.error ?? "加入协作失败"}
          }
        }.disabled(invitedChat.isEmpty || store.busy)}
      }.padding(24).frame(width:520)
    }.sheet(item:$inspecting) {job in AgentTaskMonitor(job:job)}.onAppear{store.reloadAgentJobs();if project.isEmpty {project=store.selectedProject ?? ""};if model.isEmpty {model=store.profileID};reloadMessages();reloadMembers()}
      .onChange(of:project){_,_ in sender="";receiver="";deliveryStatus="";sharedMessages=[];members=[];invitedChat="";reloadMessages();reloadMembers();if !store.chats.contains(where:{$0.id==followupChat && $0.project==project}) {followupChat=""}}
  }
}
private struct AgentMessageReceipt:Decodable {let sequence:Int64;let state:String}
private struct AgentSharedMessage:Decodable,Identifiable {let id:Int64;let sender,receiver,text:String;let delivered:Bool}
private struct AgentMember:Decodable,Identifiable {let id,title:String}

// Runtime-provided origin is copied before a synchronous approval callback.
final class AgentApprovalOrigin:@unchecked Sendable {
  private let lock=NSLock()
  private var chat:String,task:String
  init(job:AgentJob){chat=job.chat;task=job.task}
  func update(_ raw:String){
    guard let data=raw.data(using:.utf8),let value=try? JSONDecoder().decode([String:String].self,from:data),let chat=value["chat"],let task=value["task"] else{return}
    lock.lock();self.chat=chat;self.task=task;lock.unlock()
  }
  func snapshot()->(String,String){lock.lock();defer{lock.unlock()};return(chat,task)}
}
// One request per worker; the main thread presents sheets in FIFO order.
final class AgentApprovalRequest: @unchecked Sendable {
  let worker:Callbacks,job:AgentJob,tool:String,arguments:String
  let sourceChat:String,sourceTask:String
  let signal=DispatchSemaphore(value:0)
  private let lock=NSLock()
  private var result:Int32=0
  init(worker:Callbacks,job:AgentJob,tool:String,arguments:String,origin:(String,String)) {self.worker=worker;self.job=job;self.tool=tool;self.arguments=arguments;sourceChat=origin.0;sourceTask=origin.1}
  func finish(_ value:Int32) {lock.lock();result=value;lock.unlock();signal.signal()}
  func wait()->Int32 {
    while signal.wait(timeout:.now()+0.1) == .timedOut {if worker.isCancelled(){return 0}}
    lock.lock();defer{lock.unlock()};return worker.isCancelled() ? 0:result
  }
}
extension WorkspaceStore {
  private func agentWorkspace(_ job:AgentJob)->(path:String,policy:String)? {
    if let project=projects.first(where:{$0.id==job.project}) {return (project.path,project.policy)}
    guard job.native == true,let path=job.workspace,path.hasPrefix("/") else{return nil}
    return (path,job.policy ?? "ask")
  }
  // Only explicit SDK followups whose C admission is complete enter this
  // path. This never starts the user's unrelated manual project queue.
  func startNativePluginJobs() {
    startNativeModelHelpers()
    if nativeAgentEpoch != identityEpoch {nativeAgentEpoch=identityEpoch;nativeAgentReading=false;nativeAgentFailures.removeAll()}
    guard pendingIdentitySwitch == nil,!terminating,!nativeAgentReading,configurationReady,!switchingModel,!configSaving,!checkingConnection,
      !agentQueueRunning,(!busy || !agentWorkers.isEmpty),handle != nil else{return}
    nativeAgentReading=true
    readLocalView(["op":"agents.nativePending"],as:[AgentJob].self) {result in
      self.nativeAgentReading=false
      guard self.pendingIdentitySwitch == nil,result.ok,let jobs=result.data,!self.agentQueueRunning,self.configurationReady,
        !self.switchingModel,!self.configSaving,!self.checkingConnection,
        !self.busy || !self.agentWorkers.isEmpty else{return}
      for job in jobs where self.agentWorkers[job.id]==nil && !self.nativeAgentFailures.contains(job.id) {
        if self.agentWorkers.count+self.helperWorkers.count>=2 {break}
        guard let workspace=self.agentWorkspace(job),!self.agentWorkerProjects.values.contains(workspace.path),
          self.profiles.contains(where:{$0.id==job.model}) else{continue}
        self.busy=true;self.stopping=false
        self.launchAgentWorker(job,path:workspace.path,policy:workspace.policy)
      }
    }
  }
  func startAgentQueue(project:String) {
    guard !busy,configurationReady,!switchingModel,!configSaving,!checkingConnection else{return}
    agentQueueProject=project;agentQueueRunning=true;busy=true;stopping=false;agentError=""
    refreshAgentQueue()
  }
  func refreshAgentQueue() {
    let token=UUID();agentQueueReadToken=token
    readLocalView(["op":"agents.pending","project":agentQueueProject],as:[AgentJob].self) {result in
      guard self.agentQueueReadToken==token else{return}
      guard result.ok,let jobs=result.data else{self.agentQueueRunning=false;self.agentError="无法读取待执行任务。";self.fillAgentWorkers([]);return}
      self.fillAgentWorkers(jobs)
    }
  }
  func fillAgentWorkers(_ pending:[AgentJob]) {
    if agentQueueRunning {
      for job in pending where job.state=="queued" && (agentQueueProject.isEmpty || job.project==agentQueueProject) && agentWorkers[job.id]==nil {
        if agentWorkers.count+helperWorkers.count>=2 {break}
        guard let workspace=agentWorkspace(job) else{continue}
        if agentWorkerProjects.values.contains(workspace.path) {continue}
        launchAgentWorker(job,path:workspace.path,policy:workspace.policy)
      }
    }
    if agentWorkers.isEmpty {
      agentQueueRunning=false;busy = !helperWorkers.isEmpty;if !busy {stopping=false}
      reload();loadMessages();prepareIndex();reloadGrants();reloadReceipts()
    }
  }
  private func launchAgentWorker(_ job:AgentJob,path:String,policy:String) {
    guard let owner=handle,let profile=profiles.first(where:{$0.id==job.model}) else {
      agentQueueRunning=false;agentError="任务模型不可用，请在模型设置中检查。";return
    }
    let box=Callbacks(),epoch=identityEpoch,revision=configRevision
    let approvalOrigin=AgentApprovalOrigin(job:job)
    let contextPolicy=contextPreferences,knowledgePolicy=knowledgePreferences
    agentWorkers[job.id]=box;agentWorkerProjects[job.id]=path;agentRunningIDs.insert(job.id)
    box.event = {name,text in
      if name=="agent.approval" {approvalOrigin.update(text);return}
      if ["model.request","delta","chat.commit"].contains(name) {
        DispatchQueue.main.async {
          guard self.identityEpoch==epoch,self.agentWorkers[job.id] != nil else{return}
          if name=="model.request" {self.agentLiveText[job.id]="";self.agentLiveTruncated.remove(job.id)}
          else if name=="delta",!self.agentLiveTruncated.contains(job.id) {
            let current=self.agentLiveText[job.id] ?? ""
            if current.utf8.count+text.utf8.count<=131072 {self.agentLiveText[job.id]=current+text}
            else {self.agentLiveTruncated.insert(job.id)}
          }
          else if let data=text.data(using:.utf8),let commit=try? JSONDecoder().decode(ChatCommit.self,from:data),commit.chat==job.chat {
            self.agentSourceRevision[job.id,default:0]+=1
            if commit.settlesModel {self.agentLiveText[job.id]=""}
          }
        }
      }
      if ["chat.commit","agent.child.progress","tool.start","tool.result"].contains(name) {
        DispatchQueue.main.async {
          guard self.identityEpoch==epoch else{return}
          if Date().timeIntervalSince(self.agentProgressReadAt)>0.3 {self.agentProgressReadAt=Date();self.reloadAgentJobs()}
        }
      }
    }
    box.approve = {tool,args in
      let request=AgentApprovalRequest(worker:box,job:job,tool:tool,arguments:args,origin:approvalOrigin.snapshot())
      DispatchQueue.main.async {self.agentApprovalRequests.append(request);self.presentAgentApproval()}
      return request.wait()
    }
    queue.async {
      let mapped=self.coreModelSettings(owner,id:profile.id,revision:revision)
      guard var settings=mapped.data,let rawWorker=dc_open_agent_worker(owner.raw) else {
        DispatchQueue.main.async {self.finishAgentWorker(job,status:"failed",detail:"无法建立独立执行器或读取模型配置。")};return
      }
      let worker=NativeStoreReference(raw:rawWorker)
      settings["stream"]=true;settings["show_reasoning"]=true;settings["desktop_tools"]=policy
      // A scheduled task has no interactive file selection for this turn.
      settings["desktop_project_context"] = [[String: String]]()
      if knowledgePolicy.memory || knowledgePolicy.rag {settings["desktop_knowledge"]=["memory":knowledgePolicy.memory,"rag":knowledgePolicy.rag]}
      if let budget=profile.contextRange {
        settings["max_tokens"]=Int(budget.output)
        settings["desktop_context"]=contextPolicy.request(for:profile, budget:budget)
      }
      let raw=String(data:(try? JSONSerialization.data(withJSONObject:settings)) ?? Data(),encoding:.utf8) ?? "{}"
      DispatchQueue.global(qos:.userInitiated).async {
        guard case .success(let secret)=modelCredential(profile) else {
          DispatchQueue.main.async {self.finishAgentWorker(job,status:"failed",detail:"无法读取任务模型的钥匙串凭据。")};return
        }
        var hooks=dc_hooks(user:Unmanaged.passUnretained(box).toOpaque(),event:{ctx,name,data in
          guard let ctx,let name,let data else{return};Unmanaged<Callbacks>.fromOpaque(ctx).takeUnretainedValue().event(String(cString:name),String(cString:data))
        },approve:{ctx,name,args in
          guard let ctx,let name,let args else{return 0};let owner=Unmanaged<Callbacks>.fromOpaque(ctx).takeUnretainedValue();return owner.isCancelled() ? 0:owner.approve(String(cString:name),String(cString:args))
        },cancelled:{ctx in guard let ctx else{return 1};return Unmanaged<Callbacks>.fromOpaque(ctx).takeUnretainedValue().isCancelled() ? 1:0})
        var status="failed",detail="任务没有返回结果。"
        if let reply=dc_agent_execute(worker.raw,job.id,raw,secret,&hooks) {
          defer{dc_free(reply)}
          if let value=try? JSONDecoder().decode([String:String].self,from:Data(String(cString:reply).utf8)) {status=value["status"] ?? "failed";detail=value["text"] ?? ""}
        }
        let outcome=status,message=detail
        DispatchQueue.main.async {self.finishAgentWorker(job,status:outcome,detail:message)}
      }
    }
  }
  private func finishAgentWorker(_ job:AgentJob,status:String,detail:String) {
    if let (request,alert)=agentActiveApproval,request.job.id==job.id,request.worker.isCancelled() {alert.window.sheetParent?.endSheet(alert.window,returnCode:.alertSecondButtonReturn)}
    if let callbacks=agentWorkers[job.id] {callbacks.approve={_,_ in 0};callbacks.event={_,_ in}}
    agentLiveTruncated.remove(job.id);agentLiveText.removeValue(forKey:job.id);agentSourceRevision[job.id,default:0]+=1
    agentWorkers.removeValue(forKey:job.id);agentWorkerProjects.removeValue(forKey:job.id);agentRunningIDs.remove(job.id)
    if status == "unknown" { pendingIdentitySwitch = nil }
    if status != "completed" {if job.native == true {nativeAgentFailures.insert(job.id)};agentQueueRunning=false;agentError="任务 \(job.task)：\(detail.isEmpty ? status:detail)"}
    // Refresh before admission: completed jobs must not be scheduled twice.
    if agentWorkers.isEmpty && helperWorkers.isEmpty && !agentQueueRunning {busy=false;stopping=false;reload();loadMessages();prepareIndex();reloadGrants();reloadReceipts()}
    reloadAgentJobs()
    if status == "completed" {startNativePluginJobs()}
  }
  private func presentAgentApproval() {
    guard !agentApprovalPresenting else{return}
    while !agentApprovalRequests.isEmpty {
      let request=agentApprovalRequests.removeFirst()
      if request.worker.isCancelled(){request.finish(0);continue}
      guard let window=NSApp.keyWindow ?? NSApp.mainWindow ?? NSApp.windows.first(where:{$0.isVisible && $0.canBecomeMain}) else{request.finish(0);continue}
      agentApprovalPresenting=true
      let alert=NSAlert();alert.messageText="允许协作任务调用工具？"
      alert.informativeText="任务：\(request.sourceTask)\n来源聊天：\(request.sourceChat)\n工具：\(request.tool)\n本次授权不会传递给其他任务。"
      let scroll=NSScrollView(frame:NSRect(x:0,y:0,width:600,height:220)),text=NSTextView(frame:NSRect(x:0,y:0,width:600,height:220))
      text.isEditable=false;text.isSelectable=true;text.font = .monospacedSystemFont(ofSize:12,weight:.regular);text.string=request.arguments
      scroll.documentView=text;scroll.hasVerticalScroller=true;alert.accessoryView=scroll
      alert.addButton(withTitle:"仅允许这一次");alert.addButton(withTitle:"拒绝");alert.buttons[0].keyEquivalent="";alert.window.defaultButtonCell=nil
      agentActiveApproval=(request,alert)
      alert.beginSheetModal(for:window) {response in
        request.finish(response == .alertFirstButtonReturn ? 1:0);self.agentActiveApproval=nil;self.agentApprovalPresenting=false;self.presentAgentApproval()
      }
      return
    }
  }
}

struct AgentTaskMonitor:View {
  @EnvironmentObject var store:WorkspaceStore
  @Environment(\.dismiss) private var dismiss
  let job:AgentJob
  @State private var tab="chat"
  @State private var sourceSequence:Int?
  @State private var messages:[Message]=[]
  @State private var error=""
  @State private var alive=false
  @State private var loading=false
  private let tick=Timer.publish(every:1,on:.main,in:.common).autoconnect()
  var body:some View {
    VStack(alignment:.leading,spacing:16) {
      HStack {Text("任务详情").font(.title2.bold());Spacer();Text(store.agentJobs.first{$0.id==job.id}?.statusLabel ?? job.statusLabel).foregroundStyle(.secondary);Button("关闭"){dismiss()}}
      Text(job.task).textSelection(.enabled)
      Picker("任务视图",selection:$tab) {Text("聊天").tag("chat");Text("工具记录").tag("tools");Text("运行轨迹").tag("trace")}.pickerStyle(.segmented)
      if tab=="tools" {ToolRecordsView(initialChat:job.chat,onChat:{sequence in sourceSequence=sequence;tab="chat"},onTrace:{run,call in store.trajectoryRunFocus=run;store.trajectoryCallFocus=call;tab="trace"})}
      else if tab=="trace" {TrajectoryView(initialChat:job.chat,embedded:true)}
      else {
        ScrollViewReader { proxy in
        ScrollView {
          LazyVStack(alignment:.leading,spacing:18) {
            ForEach(messages.filter{["user","assistant"].contains($0.role) && !$0.content.isEmpty}) {message in
              VStack(alignment:.leading,spacing:8) {
                Text(message.role=="user" ? "你":"Dunlin Copilot").font(.caption).foregroundStyle(.secondary)
                if message.role=="assistant" {ChatHTMLContent(source:message.content)}else{Text(message.content).textSelection(.enabled)}
              }.padding(16).frame(maxWidth:.infinity,alignment:.leading).background(Color.primary.opacity(0.035),in:RoundedRectangle(cornerRadius:12)).id(message.sequence)
            }
            if let live=store.agentLiveText[job.id],!live.isEmpty {VStack(alignment:.leading,spacing:8){ChatHTMLContent(source:live);Text(store.agentLiveTruncated.contains(job.id) ? "预览已达上限 · 完整回复以保存后的记录为准":"正在生成 · 完整回复以保存后的记录为准").font(.caption).foregroundStyle(.secondary)}.padding(16)}
            if !error.isEmpty {Text(error).foregroundStyle(.orange)}
          }
        }.onAppear { if let sourceSequence { proxy.scrollTo(sourceSequence,anchor:.center) } }
        }
      }
    }.padding(24).frame(width:960,height:700)
      .onAppear{alive=true;reload()}.onDisappear{alive=false}
      .onReceive(tick){_ in if store.busy && tab=="chat" {reload()}}
      .onChange(of:store.agentSourceRevision[job.id]){_,_ in reload()}
  }
  private func reload() {
    guard alive,!loading else{return};loading=true
    store.readLocalView(["op":"chat.messages","chat":job.chat],as:[Message].self) {result in
      guard alive else{return};loading=false
      if result.ok,let rows=result.data {messages=rows;error=""}else{error="任务记录暂不可读取，请稍后重试。"}
    }
  }
}
