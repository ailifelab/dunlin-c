import SwiftUI
struct RecoveryState:Decodable {
  let stateHash:String;let active:Bool;let run:String;let profile:String;let pending:String;let arguments:String;let detail:String;let unknownCount:Int64
}
struct RecoveryView:View {
  @EnvironmentObject var store:WorkspaceStore
  @Environment(\.dismiss) private var dismiss
  @State private var state:RecoveryState?
  @State private var chatID=""
  @State private var acknowledge=false
  @State private var error=""
  @State private var busy=false
  var body:some View {
    VStack(alignment:.leading,spacing:20){
      HStack{Text("恢复与未决结果").font(.title2.weight(.semibold));Spacer();Button("完成"){dismiss()}.disabled(busy)}
      if let state {
        GroupBox {
          VStack(alignment:.leading,spacing:12){
            Label(state.active ? "原回合尚未结束":"当前没有活动回合",systemImage:state.active ? "clock.arrow.circlepath":"checkmark.circle").font(.headline)
            Text(state.detail.isEmpty ? "可继续原回合，或明确结束后开始新的消息。":state.detail).font(.callout).foregroundStyle(.secondary)
            if !state.pending.isEmpty {Text("未决调用 · \(state.pending)").font(.callout.monospaced())}
            if state.unknownCount>0 {Text("此聊天保留了 \(state.unknownCount) 条未知外部效果记录，不代表它们已成功、失败或回滚。").foregroundStyle(.orange).font(.callout)}
          }.padding(12).frame(maxWidth:.infinity,alignment:.leading)
        }
        if !state.arguments.isEmpty {Text("当时批准的最终参数").font(.headline);ScrollView{Text(state.arguments).font(.system(size:12,design:.monospaced)).textSelection(.enabled).frame(maxWidth:.infinity,alignment:.leading)}.frame(maxHeight:200)}
        if state.active {
          Text("继续会使用当时冻结的模型、提示词、预算和 Skills；未知工具调用不会自动重放。").font(.callout).foregroundStyle(.secondary)
          if !state.pending.isEmpty {
            Toggle("我理解外部效果可能已经发生，结束回合不会撤销或重试原调用",isOn:$acknowledge)
          }
          HStack {
            Button("继续原回合"){guard store.selected==chatID else{return};dismiss();store.resume()}.buttonStyle(.borderedProminent).disabled(store.busy)
            Spacer()
            Button("结束回合并保留记录",role:.destructive){abandon()}.disabled(busy || (!state.pending.isEmpty && !acknowledge))
          }
        }
      } else {ProgressView()}
      if !error.isEmpty{Text(message(error)).foregroundStyle(.red).textSelection(.enabled)}
    }.padding(28).frame(width:740,height:600).onAppear{chatID=store.selected ?? "";reload()}
      .onChange(of:store.identityEpoch){_,_ in dismiss()}
  }
  private func reload(){guard !chatID.isEmpty else{return};store.workspaceRequest(["op":"recovery.inspect","chat":chatID],as:RecoveryState.self){r in state=r.data;error=r.error ?? ""}}
  private func abandon(){guard let state else{return};busy=true;store.workspaceRequest(["op":"recovery.abandon","chat":chatID,"stateHash":state.stateHash,"acknowledgeUnknown":acknowledge],as:RecoveryState.self){r in busy=false;if r.ok{self.state=r.data;store.reload();store.prepareIndex()}else{error=r.error ?? "结束未完成"}}}
  private func message(_ code:String)->String {switch code{case "recovery_state_changed":return "回合状态已变化，请关闭后重新查看。";case "invalid_recovery_checkpoint","invalid_recovery_pairing":return "恢复记录不完整，原数据保持保留；未重新执行任何工具。";case "unknown_acknowledgement_required":return "请先确认对未知外部效果的说明。";default:return code}}
}
