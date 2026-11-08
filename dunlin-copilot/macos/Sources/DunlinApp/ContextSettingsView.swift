import SwiftUI

struct ContextSettingsView: View {
  @EnvironmentObject var store: WorkspaceStore
  @State private var records: [CompactionRecord] = []
  @State private var error = ""
  var body: some View {
    VStack(alignment:.leading,spacing:24) {
      Form {
        Section("默认整理策略") {
          Picker("算法",selection:$store.contextPreferences.strategy) {
            Text("语义摘要 + 近期原文").tag("semantic")
            Text("仅保留近期原文 · 本地 C").tag("recent")
          }.accessibilityIdentifier("context.strategy")
          Toggle("接近预算时自动整理",isOn:$store.contextPreferences.automatic)
            .accessibilityIdentifier("context.automatic")
          Text(store.contextPreferences.strategy == "semantic" ? "语义摘要会向本回合所选模型额外请求一次摘要。只保存纯文本候选，不执行摘要中提出的工具。完整原始聊天始终保留。":"本地策略不请求摘要模型，只保留最近完整回合，并在请求中明确说明更早内容未包含。完整原始聊天始终保留。")
            .font(.caption).foregroundStyle(.secondary)
        }
        Section("阈值与保留") {
          HStack {Text("开始整理：输入预算的 \(Int(store.contextPreferences.threshold*100))%");Spacer()}
          Slider(value:$store.contextPreferences.threshold,in:0.25...0.95,step:0.05)
          Stepper("保留最近 \(store.contextPreferences.retainTurns) 个完整回合",value:$store.contextPreferences.retainTurns,in:1...8)
          TextField("摘要最大输出 tokens",value:$store.contextPreferences.summaryOutput,format:.number)
            .onChange(of:store.contextPreferences.summaryOutput) {_,value in
              if value<1 {store.contextPreferences.summaryOutput=1}
              else if value>8192 {store.contextPreferences.summaryOutput=8192}
            }
          Button("配置模型窗口与上下文预算…"){store.settingsSection="模型"}
          Text("工具调用与结果成对保留，当前回合、系统指令和未确认效果不会被拆开。若保留内容本身超限，会停止并说明原因。").font(.caption).foregroundStyle(.secondary)
        }
      }.formStyle(.grouped)
      HStack {Text("当前聊天的整理记录").font(.headline);Spacer();Button("刷新",action:reload)}
      if store.selected == nil {Text("选择一段聊天后查看带来源的整理记录。").foregroundStyle(.secondary)}
      else if records.isEmpty {Text("尚无整理记录。模型需配置窗口上限，达到阈值后才会触发。").foregroundStyle(.secondary)}
      ForEach(records) { row in
        GroupBox {
          VStack(alignment:.leading,spacing:8) {
            HStack {
              Text(row.strategy == "semantic" ? "语义摘要":"近期原文").fontWeight(.semibold)
              Spacer();Text(row.status == "committed" ? "已应用到请求视图":"未应用").foregroundStyle(.secondary)
            }
            Text("\(row.beforeBytes) → \(row.afterBytes) 字节 · \(row.sourceCount) 条来源请求消息").font(.callout)
            Text("输入计量：UTF-8 字节估算；原始聊天未删减。").font(.caption).foregroundStyle(.secondary)
            Text("来源摘要：\(row.sourceHash)").font(.caption2.monospaced()).textSelection(.enabled)
            if !row.error.isEmpty {Text(row.error).font(.caption).foregroundStyle(.orange)}
          }.padding(10)
        }
      }
      if !error.isEmpty {Text(error).foregroundStyle(.red)}
    }.onAppear(perform:reload).onChange(of:store.selected){_,_ in reload()}
  }
  private func reload() {
    guard let id=store.selected else {records=[];return}
    store.workspaceRequest(["op":"context.records","chat":id],as:[CompactionRecord].self) { result in
      if result.ok {records=result.data ?? [];error=""} else {error=store.workspaceError(result.error)}
    }
  }
}
