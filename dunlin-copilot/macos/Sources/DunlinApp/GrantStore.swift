import CDesktop
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
      let values = rows.map(\.record)
      if before==0 {self.grants=values} else {self.grants.append(contentsOf:values)}
    }
  }
  func revokeGrant(_ record: GrantRecord, report: @escaping (ToolGrantReceipt) -> Void) {
    guard let id = record.recordID else { return }
    readLocalView(["op":"grants.list","recordID":id,"chat":record.chat],as:[StoredGrant].self) { reply in
      guard let current = reply.data?.first, reply.ok else {
        report(ToolGrantReceipt(grant:record, message:"这项授权暂不可读，请重新核对。")); return
      }
      self.requestGrantStop(current.record) { message in report(ToolGrantReceipt(grant:current.record,message:message)) }
    }
  }
  private func requestGrantStop(_ grant: GrantRecord, report: @escaping (String) -> Void) {
    let target=GrantCancellationTarget.resolve(status:grant.status,chat:grant.chat,
      ordinaryChat:ordinaryRunningChat,workers:Set(agentWorkers.keys),
      tasks:agentJobs.map{GrantTaskOwner(id:$0.id,chat:$0.chat,parent:$0.parent,
        active:["running","waiting","cancel_requested"].contains($0.state))})
    switch target {
    case .worker(let id):
      agentWorkers[id]?.cancel()
      report("正在停止这项授权所属的协作任务。其他任务继续运行，已产生的效果保留。");return
    case .ordinary:
      stop();report("正在停止这项授权所属的任务，等待执行结果结算。已产生的效果保留。");return
    case .unavailable:
      reloadAgentJobs();report("尚未找到这项授权所属的运行任务，已刷新状态；请重新核对。");return
    case .inactive:break
    }
    if busy {report("这次调用的授权已失效；当前执行中的任务不受影响。");return}
    guard let id=grant.recordID else{return}
    workspaceRequest(["op":"grants.revoke","id":id],as:[String:String].self) { result in
      if result.ok, result.data?["state"] == "already_inactive" {report("这次调用的授权已失效。文件和命令的历史效果保留。")}
      else if result.ok {report("这次调用仍在结算，请先停止当前任务，再核对授权状态。")}
      else {report(self.workspaceError(result.error))}
    }
  }
}
func grantStatus(_ grant:GrantRecord)->String {
  grantStatusText(status:grant.status,allowed:grant.allowed)
}
private extension StoredGrant {
  var record: GrantRecord {
    GrantRecord(chat: chat, tool: tool, arguments: arguments, allowed: allowed,
      date: Date(timeIntervalSince1970: Double(started)), recordID: id, status: status,
      waitMs: waitMs, decided: decided, ended: ended)
  }
}

struct GrantSettings: View {
  @EnvironmentObject var store: WorkspaceStore
  @State private var history = false
  @State private var result: ToolGrantReceipt?
  @State private var rows: [StoredGrant] = []
  @State private var more = false
  @State private var loading = false
  @State private var error = ""
  @State private var readID = UUID()

  private func load(before: Int64 = 0) {
    let id = UUID()
    readID = id
    loading = true
    store.readLocalView(["op": "grants.list", "scope": history ? "history" : "active",
                         "before": before, "limit": 200], as: [StoredGrant].self) { reply in
      guard readID == id else { return }
      loading = false
      guard reply.ok, let page = reply.data else {
        error = store.workspaceError(reply.error); return
      }
      error = ""
      if before == 0 { rows = page } else { rows.append(contentsOf: page) }
      more = page.count == 200
    }
  }

  var body: some View {
    VStack(alignment: .leading, spacing: 18) {
      Picker("授权状态", selection: $history) {
        Text("生效中").tag(false)
        Text("历史记录").tag(true)
      }.labelsHidden().pickerStyle(.segmented).fixedSize().accessibilityIdentifier("grant.state")
      PlatformGrantList(history: history).id(store.identityEpoch)
      ForEach(rows, id: \.id) { row in
        let grant = row.record
        HStack(spacing: 16) {
          VStack(alignment: .leading, spacing: 5) {
            Text(row.tool).fontWeight(.semibold)
            Text(store.chats.first { $0.id == row.chat }?.title ?? "所属对话")
              .font(.callout).foregroundStyle(.secondary)
            DisclosureGroup("调用参数") {
              Text(row.arguments).font(.system(.callout, design: .monospaced)).textSelection(.enabled)
            }.font(.callout)
          }
          Spacer()
          VStack(alignment: .trailing, spacing: 4) {
            Text(grantStatus(grant)).font(.callout)
            if row.allowed && row.decided > 0 {
              if row.ended > 0 { Text("授权持续 \(max(0, row.ended-row.decided)) 秒") }
              else { Text(Date(timeIntervalSince1970: Double(row.decided)), style: .timer).monospacedDigit() }
            }
            Text(grant.date, format: .dateTime).help("确认等待 " + String(format: "%.1f", Double(row.waitMs) / 1000) + " 秒")
          }.font(.caption).foregroundStyle(.secondary)
          Button(history ? "核对撤销状态" : "撤销") {
            store.revokeGrant(grant) { result = $0; load() }
          }.accessibilityIdentifier("grant.inspect.\(row.id)")
        }.padding(.horizontal, 18).padding(.vertical, 12)
          .background(store.themeSurface, in: RoundedRectangle(cornerRadius: 9))
      }
      if !error.isEmpty {
        HStack { Text(error).foregroundStyle(.red); Button("重试") { load() } }
      } else if rows.isEmpty && !loading {
        Text(history ? "没有符合当前范围的历史工具授权。" : "没有生效中的工具调用授权。")
          .font(.callout).foregroundStyle(.secondary)
      }
      if more, let last = rows.last {
        Button("加载更早的记录") { load(before: last.id) }.disabled(loading)
      }
      if let url = URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy") {
        Link("系统麦克风、摄像头与自动化权限：查看系统设置 ↗", destination: url).font(.callout)
      }
    }.onAppear { load() }
      .onChange(of: history) { _, _ in rows = []; more = false; load() }
      .onChange(of: store.identityEpoch) { _, _ in rows = []; more = false; result = nil; load() }
      .onChange(of: store.busy) { _, _ in load() }
      .onReceive(Timer.publish(every: 2, on: .main, in: .common).autoconnect()) { _ in
        if store.busy && !history && !loading && !more { load() }
      }.grantResult($result)
  }
}

struct ToolGrantReceipt: Identifiable {
  let grant: GrantRecord
  let message: String
  var id: Int64 { grant.recordID ?? -1 }
}

extension View {
  func grantResult(_ result: Binding<ToolGrantReceipt?>) -> some View {
    sheet(item: result) { ToolGrantReceiptView(receipt: $0) }
  }
}

private struct ReceiptLine: View {
  let label: String
  let value: String
  var body: some View {
    HStack(alignment: .firstTextBaseline, spacing: 24) {
      Text(label).fontWeight(.medium); Spacer(minLength: 16)
      Text(value).foregroundStyle(.secondary).multilineTextAlignment(.trailing).textSelection(.enabled)
    }.padding(.vertical, 9)
  }
}

private struct ToolGrantReceiptView: View {
  @Environment(\.dismiss) private var dismiss
  @EnvironmentObject var store: WorkspaceStore
  let receipt: ToolGrantReceipt
  @State private var current: StoredGrant?
  @State private var issue = ""
  @State private var loading = false
  @State private var stopMessage = ""
  private var grant: GrantRecord { current?.record ?? receipt.grant }
  private var active: Bool { ["awaiting","issued_once"].contains(grant.status) }
  private func refresh() {
    guard !loading else { return }; loading = true
    store.readLocalView(["op":"grants.list","recordID":receipt.id,"chat":receipt.grant.chat],as:[StoredGrant].self) { reply in
      loading = false
      if reply.ok, let saved = reply.data?.first { current = saved; issue = "" }
      else { issue = "授权记录暂不可读，请重新核对。" }
    }
  }
  var body: some View {
    VStack(alignment: .leading, spacing: 14) {
      Text("授权处理结果").font(.title2.weight(.semibold))
      Text(grant.tool).font(.headline)
      Text(active ? (stopMessage.isEmpty ? receipt.message : stopMessage) : "这次调用已经结束。已执行的文件和命令操作会保留。")
        .foregroundStyle(.secondary).fixedSize(horizontal:false,vertical:true)
      VStack(spacing: 0) {
        ReceiptLine(label:"本次应用授权",value:grantStatus(grant)); Divider()
        ReceiptLine(label:"授权范围",value:"限本次工具调用"); Divider()
        ReceiptLine(label:"开始时间",value:grant.date.formatted())
        if grant.decided > 0 {
          Divider()
          if grant.ended > 0 { ReceiptLine(label:"授权持续",value:"\(max(0,grant.ended-grant.decided)) 秒") }
          else if active { TimelineView(.periodic(from:.now,by:1)){time in
            ReceiptLine(label:"授权持续",value:"\(max(0,Int64(time.date.timeIntervalSince1970)-grant.decided)) 秒")
          }}
        }
      }.padding(.horizontal,16).background(store.themeSurface,in:RoundedRectangle(cornerRadius:10))
      if !issue.isEmpty {Text(issue).foregroundStyle(.orange)}
      HStack {
        if !issue.isEmpty { Button("重新核对",action:refresh) }
        else if active { Button("重新检查并停止") {
          store.revokeGrant(grant){ next in stopMessage=next.message;refresh() }
        }.disabled(loading) }
        Spacer();Button("返回授权记录"){dismiss()}.keyboardShortcut(.defaultAction)
      }
    }.padding(26).frame(width:540).onAppear(perform:refresh)
      .onReceive(Timer.publish(every:1,on:.main,in:.common).autoconnect()) { _ in
        if active && issue.isEmpty { refresh() }
      }
  }
}

struct PlatformGrant:Decodable,Identifiable {
  let id:String,chat:String,protocolName:String,scope:String,state:String
  let started:Int64,expires:Int64,revoked:Int64
  let generation:Int64?
  enum CodingKeys:String,CodingKey {case id,chat,scope,state,started,expires,revoked,generation;case protocolName="protocol"}
}
private struct RevocationAttempt: Decodable, Identifiable {
  let generation: Int64
  let started: Int64
  let state: String
  var id: Int64 { generation }
}

private struct RevocationHistory: View {
  @EnvironmentObject var store: WorkspaceStore
  let grant: PlatformGrant
  @State private var attempts: [RevocationAttempt] = []
  @State private var requestID = UUID()
  @State private var error = ""

  var body: some View {
    VStack(alignment: .leading, spacing: 6) {
      if !attempts.isEmpty {
        Text("撤销尝试").font(.headline)
        ScrollView {
          VStack(alignment: .leading, spacing: 6) {
            ForEach(attempts) { attempt in
              HStack {
                Text("第 \(attempt.generation) 次 · \(outcome(attempt.state))")
                Spacer()
                Text(Date(timeIntervalSince1970: Double(attempt.started)), format: .dateTime)
                  .font(.caption).foregroundStyle(.secondary)
              }.accessibilityIdentifier("grant.attempt.\(attempt.generation)")
            }
          }
        }.frame(height: min(160, CGFloat(attempts.count) * 30))
      }
      if !error.isEmpty { Text(error).font(.callout).foregroundStyle(.orange) }
    }.onChange(of: "\(grant.id)/\(grant.state)/\(grant.generation ?? 0)", initial: true) { _,_ in
      let selected=UUID();requestID=selected
      store.readLocalView(["op":"grants.platform.attempts","id":grant.id], as:[RevocationAttempt].self) { result in
        guard selected==requestID else { return }
        attempts=result.data ?? [];error=result.ok ? "" : "暂时无法读取撤销尝试。"
      }
    }
  }
  private func outcome(_ state: String) -> String {
    switch state {
    case "revoked": return grant.protocolName == "oauth-revocation" ? "服务商已确认撤销" : "系统已确认撤销"
    case "retry": return "未成功，可重试"
    case "needs_user": return "需要手动处理"
    case "revoking": return "撤销进行中"
    default: return "撤销结果待核对"
    }
  }
}

private struct RemoteAccountSecret: Codable {
  let endpoint: String
  let token: String
}

extension WorkspaceStore {
  private func accountKey(_ id: String) -> String {
    "oauth/" + (currentIdentity?.id ?? "local") + "/" + id
  }
  func recordAccount(chat: String, name: String, endpoint: String, token: String,
                     completion: @escaping (Bool) -> Void) {
    guard !busy, let handle, !token.isEmpty, !name.isEmpty,
          let data = try? JSONEncoder().encode(RemoteAccountSecret(endpoint:endpoint, token:token)),
          let secret = String(data:data,encoding:.utf8) else { completion(false); return }
    let epoch = identityEpoch, id = UUID().uuidString, key = accountKey(id)
    queue.async {
      let saved = saveCredential(key, secret)
      let ok = saved && dc_remote_account_record(handle.raw,id,chat,name,endpoint,Int64(Date().timeIntervalSince1970)+86400) != 0
      if saved && !ok { _ = saveCredential(key,"") }
      DispatchQueue.main.async { if self.identityEpoch == epoch { completion(ok) } }
    }
  }
  func revokePlatformGrant(_ row: PlatformGrant, completion: @escaping (WorkspaceReply<[PlatformGrant]>) -> Void) {
    guard row.protocolName == "oauth-revocation" else {
      workspaceRequest(["op":"grants.platform.revoke","id":row.id],as:[PlatformGrant].self,completion:completion)
      return
    }
    guard let handle else {completion(WorkspaceReply(ok:false,error:"permission_store_unavailable",data:nil));return}
    let key = accountKey(row.id), epoch = identityEpoch
    queue.async {
      let saved = (try? readCredential(key).get()).flatMap { $0.data(using:.utf8) }
        .flatMap { try? JSONDecoder().decode(RemoteAccountSecret.self,from:$0) }
      let result = self.decode(dc_remote_account_revoke(handle.raw,row.id,saved?.endpoint,saved?.token),as:[PlatformGrant].self)
      if saved != nil && result?.first(where:{$0.id==row.id})?.state=="revoked" { _ = saveCredential(key,"") }
      DispatchQueue.main.async {
        if self.identityEpoch==epoch { completion(WorkspaceReply(ok:result != nil,error:result == nil ? "remote_revoke_failed" : nil,data:result)) }
      }
    }
  }
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
  @State private var receipt:PlatformGrant?
  @State private var receiptError=""
  @State private var revoking=false
  @State private var addingAccount=false
  @State private var accountName=""
  @State private var accountEndpoint=""
  @State private var accountToken=""
  @State private var batchSelection:[PlatformGrant]=[]
  @State private var batchConfirmation=false
  @State private var batchPresented=false
  @State private var batchResults:[PermissionRevokeItem]=[]
  @State private var batchStopped=false
  @State private var alive=false
  var body:some View {
    VStack(alignment:.leading,spacing:12) {
      if let chat, !chat.isEmpty {
        Button("关联远端账号…") { addingAccount=true }.disabled(revoking || store.busy)
      }
      if !visibleRows.isEmpty {
        HStack {
          Text("账号与系统权限").font(.headline);Spacer()
          Button("批量撤销…") {
            batchSelection=visibleRows.filter{$0.state != "revoked"};batchConfirmation=true
          }.disabled(store.busy || revoking || !visibleRows.contains{$0.state != "revoked"})
        }
        Text("撤销时先停止本地授权，再向对应服务提交请求。需要重试的项目会保留在历史记录中。").font(.caption).foregroundStyle(.secondary)
        ForEach(visibleRows) {row in
          let camera=row.id.hasSuffix("-camera"),speech=row.id.hasSuffix("-speech")
          GroupBox {
            HStack {
              VStack(alignment:.leading,spacing:6) {
                Label(row.protocolName == "oauth-revocation" ? row.scope : speech ? "本地语音识别":camera ? "摄像头":"麦克风",systemImage:row.protocolName == "oauth-revocation" ? "person.crop.circle" : speech ? "text.bubble":camera ? "video":"mic")
                Text(Date(timeIntervalSince1970:Double(row.started)),format:.dateTime).font(.caption).foregroundStyle(.secondary)
                Text(state(row)).font(.caption).foregroundStyle(.secondary)
                TimelineView(.periodic(from:.now,by:1)) {timeline in
                  let now=Int64(timeline.date.timeIntervalSince1970)
                  if row.state == "active" && now<row.expires {
                    Text("本次应用授权已持续 \(max(0,now-row.started)) 秒 · 上限 \(max(0,row.expires-row.started)) 秒")
                      .font(.caption).foregroundStyle(.secondary)
                  } else if row.revoked>0 {
                    Text("撤销确认于 \(Date(timeIntervalSince1970:Double(row.revoked)).formatted())")
                      .font(.caption).foregroundStyle(.secondary)
                  } else {
                    Text("本次应用授权已结束 · 原有效期至 \(Date(timeIntervalSince1970:Double(row.expires)).formatted())")
                      .font(.caption).foregroundStyle(.secondary)
                  }
                }
              }
              Spacer()
              if row.state == "needs_user" && row.protocolName == "macos-tcc" {Button("打开系统设置") {openPrivacy(camera,speech:speech)}}
              if row.state != "active" {Button("查看结果"){receiptError="";receipt=row}.accessibilityIdentifier("grant.receipt."+row.id)}
              Button(row.state == "revoked" ? "已撤销":row.state == "retry" ? "重试撤销":"撤销权限") {pending=row}.disabled(store.busy || revoking || row.state == "revoked")
            }.padding(10)
          }
        }
      }
      if !error.isEmpty {Text(error).foregroundStyle(.orange).font(.callout)}
    }.onReceive(Timer.publish(every:1,on:.main,in:.common).autoconnect()) {date in clockNow=Int64(date.timeIntervalSince1970)}
      .onAppear{alive=true;reload()}.onDisappear{alive=false;batchStopped=true}.onChange(of:store.busy) {_,busy in if !busy {reload()}}
      .sheet(isPresented:$addingAccount,onDismiss:{accountToken="";accountName="";accountEndpoint=""}) {
        VStack(alignment:.leading,spacing:16) {
          Text("关联远端账号").font(.title2.bold())
          Text("登记这段聊天所用的账号。令牌保存在本机钥匙串，撤销时发送到你确认的服务商 HTTPS 地址。")
          TextField("账号名称",text:$accountName).accessibilityIdentifier("grant.account.name")
          TextField("服务商令牌撤销地址",text:$accountEndpoint).accessibilityIdentifier("grant.account.endpoint")
          SecureField("待撤销的 OAuth 令牌",text:$accountToken).accessibilityIdentifier("grant.account.token")
          Text("本地授权期限为 24 小时。").font(.caption)
          if !error.isEmpty {Text(error).foregroundStyle(.red)}
          HStack {Button("取消"){addingAccount=false};Spacer();Button("保存授权"){
            guard let chat else{return};revoking=true
            store.recordAccount(chat:chat,name:accountName,endpoint:accountEndpoint,token:accountToken){ok in
              revoking=false;if ok {addingAccount=false;error="";reload()}else{error="请检查账号、HTTPS 地址及钥匙串访问权限。"}
            }
          }.disabled(accountToken.isEmpty || accountName.isEmpty || !accountEndpoint.hasPrefix("https://") || revoking)}
        }.textFieldStyle(.roundedBorder).padding(24).frame(width:520)
      }
      .confirmationDialog("撤销这 \(batchSelection.count) 条权限记录？",isPresented:$batchConfirmation,titleVisibility:.visible) {
        Button("逐项撤销",role:.destructive) {
          batchResults=[];batchStopped=false;revoking=true;batchPresented=true;revokeBatch(at:0)
        }
      } message:{Text("处理当前列表中可撤销的账号与系统权限。将逐项调用对应撤销工具；需要系统操作或失败的项目会分别列出。已发生的工具效果保留。")}
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
              GroupBox {
                HStack {
                  Text(item.title)
                  Spacer()
                  Text(item.result).foregroundStyle(.secondary)
                  if item.retryable, let grant=batchSelection.first(where:{$0.id==item.id}) {
                    Button("重试撤销"){retryBatchItem(grant)}.disabled(revoking)
                  }
                }.padding(8)
              }
            }
          }}
          Text("本地不再允许使用与系统权限已撤销是不同状态。停止后续项目不会撤回已经完成的撤销。").font(.caption).foregroundStyle(.secondary)
        }.padding(24).frame(width:620,height:420).interactiveDismissDisabled(revoking)
      }
      .alert("撤销这项授权？",isPresented:Binding(get:{pending != nil},set:{if !$0 {pending=nil}}),presenting:pending) { row in
        Button("撤销",role:.destructive) {revoke(row);pending=nil}
        Button("取消",role:.cancel) {pending=nil}
      } message: { _ in Text("将停止本地授权并调用对应的撤销服务。已经发生的操作会保留。")}
      .sheet(item:$receipt) { row in
        let remote=row.protocolName=="oauth-revocation"
        let current=rows.first(where:{$0.id==row.id}) ?? row
        VStack(alignment:.leading,spacing:16) {
          Text(remote ? "远端账号撤销结果" : "系统授权撤销结果").font(.title2.weight(.semibold))
          Text(permissionTitle(current)).foregroundStyle(.secondary)
          VStack(spacing:0) {
            ReceiptLine(label:"本次应用授权",value:current.state=="active" && clockNow<current.expires ? "生效中" : "已停止");Divider()
            ReceiptLine(label:remote ? "远端服务" : "系统权限",value:state(current));Divider()
            ReceiptLine(label:"开始时间",value:Date(timeIntervalSince1970:Double(current.started)).formatted())
            if current.revoked>0 {
              Divider();ReceiptLine(label:"撤销确认",value:Date(timeIntervalSince1970:Double(current.revoked)).formatted())
            }
            Divider();ReceiptLine(label:"原授权时限",value:"\(max(0,current.expires-current.started)) 秒")
          }.padding(.horizontal,16).background(store.themeSurface,in:RoundedRectangle(cornerRadius:10))
          RevocationHistory(grant:current)
          if !receiptError.isEmpty {Text(receiptError).foregroundStyle(.orange)}
          if remote && current.state=="needs_user" {
            Text("请在服务商账号的授权管理页面移除本应用。").foregroundStyle(.secondary)
          }
          HStack {
            if current.state=="needs_user" && !remote {
              Button("打开系统设置") {openPrivacy(current.id.hasSuffix("-camera"),speech:current.id.hasSuffix("-speech"))}
            }
            if current.state != "revoked" && current.state != "revoking" {
              Button("重试撤销"){revoke(current)}.disabled(store.busy || revoking).accessibilityIdentifier("grant.receipt.retry")
            }
            if revoking {ProgressView().controlSize(.small)}
            Spacer();Button("返回授权记录"){receipt=nil}.disabled(revoking).keyboardShortcut(.defaultAction)
          }
        }.padding(26).frame(width:560).interactiveDismissDisabled(revoking)
      }
  }
  private func revokeBatch(at index:Int) {
    guard alive else{revoking=false;return}
    guard index<batchSelection.count else{revoking=false;reload();return}
    if batchStopped {
      batchResults += batchSelection.dropFirst(index).map{PermissionRevokeItem(id:$0.id,title:permissionTitle($0),result:"未执行 · 已停止后续项目")}
      revoking=false;reload();return
    }
    let row=batchSelection[index]
    store.revokePlatformGrant(row) {r in
      let updated=r.data?.first{$0.id==row.id}
      let message:String
      if !r.ok {message=r.error == "stop_media_before_revoking" ? "未执行 · 请先结束通话":"未确认撤销 · 请检查后重试"}
      else if let updated {message=state(updated)}
      else {message="结果无法读取 · 请重新核对"}
      batchResults.append(PermissionRevokeItem(id:row.id,title:permissionTitle(row),result:message,retryable:updated?.state == "retry" || !r.ok))
      revokeBatch(at:index+1)
    }
  }
  private func retryBatchItem(_ row:PlatformGrant) {
    guard !revoking else{return}
    revoking=true
    store.revokePlatformGrant(row){reply in
      if let index=batchResults.firstIndex(where:{$0.id==row.id}) {
        let updated=reply.data?.first(where:{$0.id==row.id})
        batchResults[index]=PermissionRevokeItem(id:row.id,title:permissionTitle(row),
          result:updated.map(state) ?? "未确认撤销 · 请检查后重试",retryable:updated?.state == "retry" || !reply.ok)
      }
      revoking=false
      reload()
    }
  }
  private func permissionTitle(_ row:PlatformGrant)->String {
    let name=row.protocolName == "oauth-revocation" ? row.scope : row.id.hasSuffix("-speech") ? "本地语音识别":row.id.hasSuffix("-camera") ? "摄像头":"麦克风"
    return "\(name) · \(Date(timeIntervalSince1970:Double(row.started)).formatted())"
  }
  private func state(_ row:PlatformGrant)->String {
    if row.protocolName == "oauth-revocation" {
      switch row.state {
      case "revoked":return "服务商已确认撤销"
      case "active":return clockNow<row.expires ? "本地授权生效中":"本地授权已过期"
      case "needs_user":return "本地已停用 · 需在服务商处理"
      case "retry":return "本地已停用 · 远端撤销待重试"
      case "unknown","revoking":return "本地已停用 · 远端结果待确认"
      default:return "本地授权已结束 · 可继续撤销远端令牌"
      }
    }
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
  private func reload() {store.readLocalView(["op":"grants.platform.list"],as:[PlatformGrant].self) {r in if r.ok {rows=(r.data ?? []).filter{chat == nil || $0.chat == chat}}else {error="系统权限记录暂不可读取。"}}}
  private func revoke(_ row:PlatformGrant) {
    revoking=true;error="";receiptError=""
    store.revokePlatformGrant(row) { reply in
      revoking=false
      if reply.ok, let updated=reply.data?.first(where:{$0.id==row.id}) {
        rows=(reply.data ?? []).filter{chat==nil || $0.chat==chat};receipt=updated
      } else {
        receiptError=reply.error=="stop_media_before_revoking" ? "请先结束正在进行的通话，再重试撤销。" : "撤销结果未确认，请检查系统或钥匙串后重试。"
        receipt=row
        reload()
      }
    }
  }
  private func openPrivacy(_ camera:Bool,speech:Bool) {if let url=URL(string:"x-apple.systempreferences:com.apple.preference.security?\(speech ? "Privacy_SpeechRecognition":camera ? "Privacy_Camera":"Privacy_Microphone")") {NSWorkspace.shared.open(url)}}
}

private struct PermissionRevokeItem:Identifiable {let id:String;let title:String;let result:String;var retryable=false}
