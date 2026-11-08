import SwiftUI
import AVFoundation
import AppKit
import Speech

extension WorkspaceStore {
  func reloadGrants(chat: String = "", before: Int64 = 0) {
    let token=UUID();grantRequestID=token
    readLocalView(["op":"grants.list","chat":chat,"before":before,"limit":200],as:[StoredGrant].self) { result in
      guard self.grantRequestID==token else{return}
      guard result.ok,let rows=result.data else {self.error=self.workspaceError(result.error);return}
      let values=rows.map { row in
        GrantRecord(chat:row.chat,tool:row.tool,arguments:row.arguments,allowed:row.allowed,
          date:Date(timeIntervalSince1970:Double(row.started)),recordID:row.id,status:row.status,waitMs:row.waitMs,decided:row.decided,ended:row.ended)
      }
      if before==0 {self.grants=values} else {self.grants.append(contentsOf:values)}
    }
  }
  func revokeGrant(_ grant: GrantRecord) {
    let target=GrantCancellationTarget.resolve(status:grant.status,chat:grant.chat,
      ordinaryChat:ordinaryRunningChat,workers:Set(agentWorkers.keys),
      tasks:agentJobs.map{GrantTaskOwner(id:$0.id,chat:$0.chat,parent:$0.parent,
        active:["running","waiting","cancel_requested"].contains($0.state))})
    switch target {
    case .worker(let id):
      agentWorkers[id]?.cancel()
      error="正在停止这项授权所属的协作任务。其他任务继续运行，已产生的效果保留。";return
    case .ordinary:
      stop();error="正在停止这项授权所属的任务，等待执行结果结算。已产生的效果保留。";return
    case .unavailable:
      reloadAgentJobs();error="尚未找到这项授权所属的运行任务，已刷新状态；请重新核对。";return
    case .inactive:break
    }
    if busy {error="这次调用的授权已失效；当前执行中的任务不受影响。";return}
    guard let id=grant.recordID else{return}
    workspaceRequest(["op":"grants.revoke","id":id],as:[String:String].self) { result in
      if result.ok, result.data?["state"] == "already_inactive" {self.error="这次调用的授权已失效。文件和命令的历史效果保留。"}
      else if result.ok {self.error="这次调用仍在结算，请先停止当前任务，再核对授权状态。"}
      else {self.error=self.workspaceError(result.error)}
    }
  }
}
func grantStatus(_ grant:GrantRecord)->String {
  grantStatusText(status:grant.status,allowed:grant.allowed)
}
struct GrantSettings: View {
  @EnvironmentObject var store: WorkspaceStore
  @State private var history=false
  private var visibleGrants:[GrantRecord] {store.grants.filter {history != ["awaiting","issued_once"].contains($0.status)}}
  var body: some View {
    VStack(alignment:.leading,spacing:20) {
      Text("记录当前身份在对话中作出的授权。单次授权在调用结束后失效，不代表工具执行成功。").foregroundStyle(.secondary)
      HStack(spacing:28) {
        Button("生效中") {history=false}.font(.headline).foregroundStyle(history ? .secondary:.primary)
        Button("历史记录") {history=true}.font(.headline).foregroundStyle(history ? .primary:.secondary)
        Spacer()
        if store.busy {Button("撤销当前调用并停止任务",role:.destructive,action:store.stop)}
      }.buttonStyle(.plain)
      PlatformGrantList(history:history)
      ForEach(visibleGrants) { grant in
        GroupBox {
          VStack(alignment:.leading,spacing:10) {
            HStack {
              Text(grant.tool).fontWeight(.semibold);Spacer();Text(grantStatus(grant)).foregroundStyle(.secondary)
            }
            Text(grant.arguments).font(.system(size:12,design:.monospaced)).textSelection(.enabled)
            HStack {
              Text(grant.date,style:.date);Text(grant.date,style:.time)
              Spacer()
              if grant.allowed, grant.decided > 0 {
                if grant.ended > 0 {Text("授权持续 \(max(0,grant.ended-grant.decided)) 秒")}
                else {Text(Date(timeIntervalSince1970:Double(grant.decided)),style:.timer).monospacedDigit()}
              }
              Text("确认等待 \(String(format:"%.1f",Double(grant.waitMs)/1000)) 秒")
              Button("核对撤销状态") {store.revokeGrant(grant)}.disabled(grant.recordID == nil)
            }.font(.caption).foregroundStyle(.secondary)
          }.padding(10)
        }
      }
      if visibleGrants.isEmpty {Text(history ? "没有符合当前范围的历史工具授权。":"没有生效中的工具调用授权。").font(.callout).foregroundStyle(.secondary).padding(.vertical,16)}
      if let id=store.grants.last?.recordID,store.grants.count>=200 {
        Button("加载更早的记录"){store.reloadGrants(before:id)}
      }
      Text("这里不声称管理整台电脑或远端服务的所有权限。单次授权失效不会撤销已经执行的外部效果。").font(.caption).foregroundStyle(.secondary)
    }.onAppear{store.reloadGrants()}
  }
}

struct PlatformGrant:Decodable,Identifiable {
  let id:String,chat:String,protocolName:String,scope:String,state:String
  let started:Int64,expires:Int64,revoked:Int64
  enum CodingKeys:String,CodingKey {case id,chat,scope,state,started,expires,revoked;case protocolName="protocol"}
}
struct PlatformGrantList:View {
  var chat:String?=nil
  var history:Bool?=nil
  @State private var clockNow=Int64(Date().timeIntervalSince1970)
  private var visibleRows:[PlatformGrant] {rows.filter {row in
    guard let history else{return true}
    return history != (row.state == "active" && clockNow<row.expires)
  }}
  @EnvironmentObject var store:WorkspaceStore
  @State private var rows:[PlatformGrant]=[]
  @State private var error=""
  @State private var pending:PlatformGrant?
  @State private var revoking=false
  @State private var batchSelection:[PlatformGrant]=[]
  @State private var batchConfirmation=false
  @State private var batchPresented=false
  @State private var batchResults:[PermissionRevokeItem]=[]
  @State private var batchStopped=false
  @State private var alive=false
  var body:some View {
    VStack(alignment:.leading,spacing:12) {
      if !visibleRows.isEmpty {
        HStack {
          Text("通话使用的系统权限").font(.headline);Spacer()
          Button("批量撤销…") {
            batchSelection=visibleRows.filter{$0.state != "revoked"};batchConfirmation=true
          }.disabled(store.busy || revoking || !visibleRows.contains{$0.state != "revoked"})
        }
        Text("此处只列出本应用通话使用过的权限。挂断停止采集；撤销会重置整个客户端对应的系统权限，下次使用需再次授权。").font(.caption).foregroundStyle(.secondary)
        ForEach(visibleRows) {row in
          let camera=row.id.hasSuffix("-camera"),speech=row.id.hasSuffix("-speech")
          GroupBox {
            HStack {
              VStack(alignment:.leading,spacing:6) {
                Label(speech ? "本地语音识别":camera ? "摄像头":"麦克风",systemImage:speech ? "text.bubble":camera ? "video":"mic")
                Text(Date(timeIntervalSince1970:Double(row.started)),format:.dateTime).font(.caption).foregroundStyle(.secondary)
                Text(state(row)).font(.caption).foregroundStyle(.secondary)
                TimelineView(.periodic(from:.now,by:1)) {timeline in
                  let now=Int64(timeline.date.timeIntervalSince1970)
                  if row.state == "active" && now<row.expires {
                    Text("本次应用授权已持续 \(max(0,now-row.started)) 秒 · 上限 \(max(0,row.expires-row.started)) 秒")
                      .font(.caption).foregroundStyle(.secondary)
                  } else if row.revoked>0 {
                    Text("系统撤销记录于 \(Date(timeIntervalSince1970:Double(row.revoked)).formatted())")
                      .font(.caption).foregroundStyle(.secondary)
                  } else {
                    Text("本次应用授权已结束 · 原有效期至 \(Date(timeIntervalSince1970:Double(row.expires)).formatted())")
                      .font(.caption).foregroundStyle(.secondary)
                  }
                }
              }
              Spacer()
              if row.state == "needs_user" {Button("打开系统设置") {openPrivacy(camera,speech:speech)}}
              Button(row.state == "revoked" ? "已撤销":"撤销权限") {pending=row}.disabled(store.busy || revoking || row.state == "revoked")
            }.padding(10)
          }
        }
      }
      if !error.isEmpty {Text(error).foregroundStyle(.orange).font(.callout)}
    }.onReceive(Timer.publish(every:1,on:.main,in:.common).autoconnect()) {date in clockNow=Int64(date.timeIntervalSince1970)}
      .onAppear{alive=true;reload()}.onDisappear{alive=false;batchStopped=true}.onChange(of:store.busy) {_,busy in if !busy {reload()}}
      .confirmationDialog("撤销这 \(batchSelection.count) 条权限记录？",isPresented:$batchConfirmation,titleVisibility:.visible) {
        Button("逐项撤销",role:.destructive) {
          batchResults=[];batchStopped=false;revoking=true;batchPresented=true;revokeBatch(at:0)
        }
      } message:{Text("仅处理当前列表中显示的系统权限记录。将逐项调用对应撤销工具；需要系统操作或失败的项目会分别列出。已发生的工具效果保留。")} 
      .sheet(isPresented:$batchPresented) {
        VStack(alignment:.leading,spacing:18) {
          HStack {Text("批量撤销结果").font(.title2.bold());Spacer()
            if revoking {Button("停止后续项目"){batchStopped=true}.disabled(batchStopped)}
            Button("完成"){batchPresented=false}.disabled(revoking)
          }
          Text("已结算 \(batchResults.count) / \(batchSelection.count) 项").foregroundStyle(.secondary)
          if revoking {ProgressView(batchStopped ? "等待当前项目结算…":"正在逐项撤销…")}
          ScrollView {VStack(alignment:.leading,spacing:12) {
            ForEach(batchResults) {item in
              GroupBox {HStack {Text(item.title);Spacer();Text(item.result).foregroundStyle(.secondary)}.padding(8)}
            }
          }}
          Text("本地不再允许使用与系统权限已撤销是不同状态。停止后续项目不会撤回已经完成的撤销。").font(.caption).foregroundStyle(.secondary)
        }.padding(24).frame(width:620,height:420).interactiveDismissDisabled(revoking)
      }
      .alert("撤销这项系统权限？",isPresented:Binding(get:{pending != nil},set:{if !$0 {pending=nil}})) {
        Button("撤销",role:.destructive) {if let item=pending {revoke(item)};pending=nil}
        Button("取消",role:.cancel) {pending=nil}
      } message: {Text("将停止本客户端后续使用这项设备权限。已保存的通话记录不会删除；不会重置其他应用的授权。")} 
  }
  private func revokeBatch(at index:Int) {
    guard alive else{revoking=false;return}
    guard index<batchSelection.count else{revoking=false;reload();return}
    if batchStopped {
      batchResults += batchSelection.dropFirst(index).map{PermissionRevokeItem(id:$0.id,title:permissionTitle($0),result:"未执行 · 已停止后续项目")}
      revoking=false;reload();return
    }
    let row=batchSelection[index]
    store.workspaceRequest(["op":"grants.platform.revoke","id":row.id],as:[PlatformGrant].self) {r in
      let updated=r.data?.first{$0.id==row.id}
      let message:String
      if !r.ok {message=r.error == "stop_media_before_revoking" ? "未执行 · 请先结束通话":"未确认撤销 · 请检查后重试"}
      else if let updated {message=state(updated)}
      else {message="结果无法读取 · 请重新核对"}
      batchResults.append(PermissionRevokeItem(id:row.id,title:permissionTitle(row),result:message))
      revokeBatch(at:index+1)
    }
  }
  private func permissionTitle(_ row:PlatformGrant)->String {
    let name=row.id.hasSuffix("-speech") ? "本地语音识别":row.id.hasSuffix("-camera") ? "摄像头":"麦克风"
    return "\(name) · \(Date(timeIntervalSince1970:Double(row.started)).formatted())"
  }
  private func state(_ row:PlatformGrant)->String {
    switch row.state {case "revoked":return "系统权限重置命令已成功返回"
    case "needs_user":return "系统要求手动操作，请在隐私与安全中处理"
    case "retry":return "撤销未成功，可重试"
    case "unknown":return "撤销结果未知，请核对系统权限"
    case "revoking":return "正在撤销"
    default:
      if row.id.hasSuffix("-speech") {return SFSpeechRecognizer.authorizationStatus() == .authorized ? "系统语音识别权限已授权":"系统语音识别权限未授权"}
      let status=AVCaptureDevice.authorizationStatus(for:row.id.hasSuffix("-camera") ? .video:.audio)
      return status == .authorized ? "系统权限仍已授权":"系统权限当前未授权"
    }
  }
  private func reload() {store.workspaceRequest(["op":"grants.platform.list"],as:[PlatformGrant].self) {r in if r.ok {rows=(r.data ?? []).filter{chat == nil || $0.chat == chat}}else {error="系统权限记录暂不可读取。"}}}
  private func revoke(_ row:PlatformGrant) {revoking=true;error="";store.workspaceRequest(["op":"grants.platform.revoke","id":row.id],as:[PlatformGrant].self) {r in revoking=false;if r.ok {rows=(r.data ?? []).filter{chat == nil || $0.chat == chat}}else {error="权限未撤销，请先结束通话，再核对系统设置。"}}}
  private func openPrivacy(_ camera:Bool,speech:Bool) {if let url=URL(string:"x-apple.systempreferences:com.apple.preference.security?\(speech ? "Privacy_SpeechRecognition":camera ? "Privacy_Camera":"Privacy_Microphone")") {NSWorkspace.shared.open(url)}}
}

private struct PermissionRevokeItem:Identifiable {let id:String;let title:String;let result:String}
