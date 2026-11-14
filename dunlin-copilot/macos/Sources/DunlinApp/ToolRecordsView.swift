import AppKit
import Combine
import SwiftUI

struct ToolRecord: Decodable, Identifiable {
  let id: Int64
  let chat: String
  let run: String
  let sourceSequence: Int
  let callID: String
  let name: String
  let arguments: String
  let result: String?
  let resultSequence: Int?
  let stage: String
  let outcome: String?
  let grantID: Int64?
  let waitMs: Int64?
  let started: Int64?
  let executionMs: Int64?
  let committedMs: Int64?
  let created: Int64
  let historical: Bool
  let approvedArguments: String?
  let grantStatus: String?
  let fileReceiptID: String?
  let unknownEffect: Bool?
  let schema: String?
  var status: String {
    if unknownEffect == true || stage == "unknown" { return "效果未知" }
    switch stage {
    case "proposed": return "已提议"
    case "validating": return "执行前检查"
    case "awaiting": return "等待授权"
    case "denied": return "授权已拒绝"
    case "running": return "正在执行"
    case "returned": return "已返回，待提交"
    case "not_executed": return "未执行"
    case "interrupted": return "已中断 · 结果未确认"
    case "recorded": return "历史结果 · 状态未记录"
    case "committed":
      switch outcome {
      case "completed": return "已完成"
      case "execution_failed": return "命令执行失败"
      case "cancelled": return "已停止"
      case "timeout": return "执行超时"
      default: return outcome == nil ? "结果已提交" : "执行失败"
      }
    default: return stage
    }
  }
  var color: Color {
    if stage == "unknown" || stage == "interrupted" || (outcome != nil && outcome != "completed") {
      return .orange
    }
    if stage == "committed" { return .green }
    return stage == "denied" ? .secondary : .accentColor
  }
  var target: String {
    guard let bytes = arguments.data(using: .utf8),
      let json = (try? JSONSerialization.jsonObject(with: bytes)) as? [String: Any]
    else { return arguments }
    for key in ["description", "path", "file_path", "command", "query", "pattern"] {
      if let text = json[key] as? String, !text.isEmpty { return text }
    }
    return arguments
  }
  var symbol: String {
    if name.contains("write") || name.contains("patch") { return "doc.badge.gearshape" }
    if name.contains("read") { return "doc.text.magnifyingglass" }
    if name.contains("shell") || name.contains("exec") || name.contains("bash") {
      return "terminal"
    }
    if name.contains("search") { return "magnifyingglass" }
    return "wrench.and.screwdriver"
  }
}
private func toolDuration(_ n: Int64) -> String {
  n < 1000 ? "\(n) ms" : String(format: "%.2f s", Double(n) / 1000)
}

struct ToolChatCard: View {
  @EnvironmentObject var store: WorkspaceStore
  let message: Message
  @State private var record: ToolRecord?
  var body: some View {
    Button {
      if let sequence = message.sequence { store.inspectTool(sequence: sequence) }
    } label: {
      HStack(spacing: 12) {
        Image(systemName: record?.symbol ?? "wrench.and.screwdriver").foregroundStyle(
          record?.color ?? .secondary)
        VStack(alignment: .leading, spacing: 4) {
          Text(record?.name ?? "工具调用").font(.callout.weight(.medium))
          Text(record?.target ?? (message.observation == "unknown" ? "效果未知" : "查看调用参数与结果"))
            .font(.caption).foregroundStyle(.secondary).lineLimit(1)
        }
        Spacer()
        Text(record?.status ?? "查看记录").font(.caption).foregroundStyle(record?.color ?? .secondary)
        Image(systemName: "arrow.up.right.square").font(.caption).foregroundStyle(.secondary)
      }.padding(12).background(Color.primary.opacity(0.025), in: RoundedRectangle(cornerRadius: 10))
    }.buttonStyle(.plain).accessibilityIdentifier("chat.tool.\(message.sequence ?? -1)")
      .task(id: message.sequence) {
        guard let sequence = message.sequence, let chat = store.selected else { return }
        store.readLocalView(
          ["op": "tools.locate", "chat": chat, "sequence": sequence],
          as: TrajectoryPage<ToolRecord>.self
        ) { reply in
          if store.selected == chat { record = reply.data?.items.first }
        }
      }
  }
}

extension WorkspaceStore {
  func openToolRecords() {
    guard !creatingChat, !switchingModel, !workspaceTransitioning, !attachmentInteraction else { return }
    if !busy, let selected { persistDraft(selected) }
    route = "tools"
  }
  func inspectTool(sequence: Int) {
    toolFocusSequence = sequence
    openToolRecords()
  }
  func jumpToToolSource(_ sequence: Int) {
    guard let chat = selected else { return }
    if busy {
      // The ordered live commit reader owns the current transcript; an
      // unbounded asynchronous read could include a future stream settlement.
      route = "chat"
      conversationPage = "chat"
      jumpToMessage = sequence
      return
    }
    let stamp = chatStreamCursor.stamp
    readLocalView(["op": "chat.messages", "chat": chat], as: [Message].self) { reply in
      guard self.selected == chat, !self.busy, self.chatStreamCursor.stamp == stamp else { return }
      if reply.ok, let messages = reply.data {
        self.messages = messages
        self.route = "chat"
        self.conversationPage = "chat"
        self.jumpToMessage = sequence
      } else {
        self.error = reply.error ?? "原文暂不可读，请刷新后重试。"
      }
    }
  }
}

struct ToolRecordsView: View {
  var initialChat = ""
  var onChat: ((Int?) -> Void)? = nil
  var onTrace: ((String, String) -> Void)? = nil
  @EnvironmentObject var store: WorkspaceStore
  @State private var chat = ""
  @State private var records: [ToolRecord] = []
  @State private var embeddedReceipt: FileReceiptSelection?
  @State private var selectedID: Int64?
  @State private var detail: ToolRecord?
  @State private var more = false
  @State private var cursor: Int64 = 0
  @State private var query = ""
  @State private var stage = ""
  @State private var error = ""
  @State private var loading = false
  @State private var requestID = UUID()
  @State private var detailRequestID = UUID()
  @State private var tab = "summary"
  @State private var followsTail = true
  @State private var initialized = false
  private let tick = Timer.publish(every: 1.2, on: .main, in: .common).autoconnect()
  var body: some View {
    ZStack {
      recordsBody.opacity(embeddedReceipt == nil ? 1 : 0)
        .allowsHitTesting(embeddedReceipt == nil).accessibilityHidden(embeddedReceipt != nil)
      if let selection = embeddedReceipt {
        FileReceiptView(selection: selection) { embeddedReceipt = nil }.id(selection.id)
      }
    }
  }
  private var recordsBody: some View {
    VStack(spacing: 0) {
      HStack(spacing: 14) {
        Text("工具执行记录").font(.title2.weight(.semibold))
        Spacer()
        if let onChat {
          Button("返回聊天") { onChat(nil) }.accessibilityIdentifier("tools.backToChat")
        } else if let selected = store.selected {
          Button("返回聊天") {
            if store.busy { store.route = "chat"; store.conversationPage = "chat" }
            else { store.openChat(selected) }
          }.accessibilityIdentifier("tools.backToChat")
        }
        if store.busy {
          Label("实时更新", systemImage: "circle.fill").font(.caption).foregroundStyle(.secondary)
        }
        Button {
          load()
        } label: {
          Label("刷新", systemImage: "arrow.clockwise")
        }
        .accessibilityIdentifier("tools.refresh")
      }.padding(.horizontal, 24).padding(.vertical, 20)
      if let selected = store.chats.first(where: {$0.id == chat}) {
        Text("来源聊天 · \(selected.title)").font(.callout).foregroundStyle(.secondary)
          .frame(maxWidth: .infinity, alignment: .leading).padding(.horizontal, 24).padding(.bottom, 12)
      }
      HStack(spacing: 12) {
        TextField("查找工具、文件、命令或结果", text: $query).textFieldStyle(.roundedBorder)
          .onSubmit { resetAndLoad() }.accessibilityIdentifier("tools.search")
        Picker("状态", selection: $stage) {
          Text("全部状态").tag("")
          Text("等待授权").tag("awaiting")
          Text("运行中").tag("running")
          Text("结果已提交").tag("committed")
          Text("已拒绝").tag("denied")
          Text("未执行").tag("not_executed")
          Text("效果未知").tag("unknown")
          Text("已中断").tag("interrupted")
        }.labelsHidden().frame(width: 140).accessibilityIdentifier("tools.filter")
        Button("查找") { resetAndLoad() }
      }.padding(.horizontal, 24).padding(.bottom, 16)
      Divider()
      HSplitView {
        VStack(spacing: 0) {
          if records.isEmpty {
            if loading {
              ProgressView("正在读取执行记录…").frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
              ContentUnavailableView(
                query.isEmpty && stage.isEmpty ? "还没有工具执行记录" : "没有匹配的工具记录",
                systemImage: "wrench.and.screwdriver",
                description: Text(store.selected == nil ? "先从侧栏选择一段聊天，再打开工具记录。" : "工具调用记录与当前聊天关联。普通聊天可以不使用工具。"))
            }
          } else {
            ScrollViewReader { proxy in
              ScrollView {
                LazyVStack(spacing: 0) {
                  if more { Button("加载更早的调用") { load(older: true) }.padding(14).disabled(loading) }
                  ForEach(records.sorted { $0.id < $1.id }) { record in
                    Button {
                      selectedID = record.id
                      followsTail = false
                    } label: {
                      recordRow(record)
                    }
                    .buttonStyle(.plain).id(record.id).accessibilityIdentifier(
                      "tools.call.\(record.id)")
                    Divider().padding(.leading, 48)
                  }
                  Color.clear.frame(height: 1).id("tool-tail")
                }
              }.onScrollPhaseChange { _, phase in if phase == .interacting { followsTail = false } }
                .onChange(of: records.map(\.id)) { _, _ in
                  if followsTail { proxy.scrollTo("tool-tail", anchor: .bottom) }
                }
                .onChange(of: selectedID) { _, id in
                  if let id { proxy.scrollTo(id, anchor: .center) }
                }
            }
          }
          if !followsTail && store.busy {
            Button("跟随最新调用") {
              followsTail = true
              load()
            }.padding(10)
          }
        }.frame(minWidth: 300, maxWidth: .infinity, maxHeight: .infinity)
        inspector.frame(minWidth: 320, idealWidth: 360, maxWidth: 480, maxHeight: .infinity)
          .background(Color(nsColor: .controlBackgroundColor))
      }
      if !error.isEmpty {
        Text(error).font(.callout).foregroundStyle(.orange).textSelection(.enabled).padding(12)
          .frame(maxWidth: .infinity, alignment: .leading)
      }
    }.background(Color(nsColor: .textBackgroundColor))
      .onAppear {
        chat = initialChat.isEmpty ? (store.selected ?? ""):initialChat
        load()
        locate()
      }
      .onChange(of: store.selected) { _, value in
        chat = initialChat.isEmpty ? (value ?? ""):initialChat
        resetAndLoad()
        locate()
      }
      .onChange(of: stage) { _, _ in resetAndLoad() }
      .onChange(of: selectedID) { _, _ in
        tab = "summary"
        loadDetail()
      }
      .onChange(of: store.toolFocusSequence) { _, _ in locate() }
      .onChange(of: store.busy) { _, busy in
        if !busy {
          load()
          loadDetail()
        }
      }
      .onReceive(tick) { _ in
        if store.busy {
          load()
          loadDetail()
        }
      }
  }
  private func recordRow(_ record: ToolRecord) -> some View {
    HStack(alignment: .top, spacing: 12) {
      Image(systemName: record.symbol).font(.headline).foregroundStyle(record.color).frame(
        width: 20
      ).padding(.top, 2)
      VStack(alignment: .leading, spacing: 7) {
        HStack {
          Text(record.name).font(.callout.weight(.semibold))
          Spacer()
          Text(record.status).font(.caption).foregroundStyle(record.color)
        }
        Text(record.target).font(.system(.callout, design: .monospaced)).lineLimit(2)
          .foregroundStyle(.secondary)
        HStack(spacing: 12) {
          Text(record.historical ? "早期历史记录" : "回合 \(record.run.prefix(8))")
          if let duration = record.executionMs { Text(toolDuration(duration)) }
          if record.grantID != nil {
            Label("已记录授权", systemImage: "lock.shield").labelStyle(.titleOnly)
          }
        }.font(.caption2).foregroundStyle(.tertiary)
      }
    }.padding(.horizontal, 18).padding(.vertical, 14).contentShape(Rectangle())
      .background(selectedID == record.id ? Color.accentColor.opacity(0.09) : .clear)
  }
  @ViewBuilder private var inspector: some View {
    if let detail {
      VStack(alignment: .leading, spacing: 16) {
        HStack {
          Image(systemName: detail.symbol).foregroundStyle(detail.color)
          Text(detail.name).font(.headline)
          Spacer()
          Text(detail.status).font(.caption).foregroundStyle(detail.color)
        }
        Picker("详情", selection: $tab) {
          Text("概览").tag("summary")
          Text("参数").tag("arguments")
          Text("结果").tag("result")
          Text("定义").tag("schema")
          Text("时间").tag("timing")
        }.pickerStyle(.segmented).labelsHidden().accessibilityIdentifier("tools.detail.tabs")
        ScrollView {
          VStack(alignment: .leading, spacing: 18) {
            if tab == "summary" {
              LabeledContent("工具", value: detail.name)
              LabeledContent("结果提交", value: detail.resultSequence == nil ? "未确认" : "已持久保存")
              LabeledContent("调用 ID", value: detail.callID).textSelection(.enabled)
              if detail.historical {
                Text("早期历史没有完整的执行状态与时间记录；有结果文本不等于工具执行成功。").foregroundStyle(.secondary)
              }
              if detail.unknownEffect == true || detail.stage == "unknown" {
                Label("外部效果尚未确认。停止或结束回合不会撤销已经发生的修改。", systemImage: "exclamationmark.triangle")
                  .foregroundStyle(.orange)
              }
              if detail.grantID != nil {
                LabeledContent("对话授权", value: detail.stage == "denied" ? "已拒绝" : "单次授权")
                if let status = detail.grantStatus {
                  LabeledContent("授权状态", value: grantLabel(status))
                }
              }
              Text("参数").font(.subheadline.weight(.medium))
              code(detail.arguments)
              if let result = detail.result {
                Text("结果").font(.subheadline.weight(.medium))
                code(result)
              }
            } else if tab == "arguments" {
              Text("原始调用参数").font(.subheadline.weight(.medium))
              code(detail.arguments)
              if let approved = detail.approvedArguments, approved != detail.arguments {
                Text("实际批准的参数").font(.subheadline.weight(.medium))
                code(approved)
              }
            } else if tab == "result" {
              if let result = detail.result {
                code(result)
              } else {
                Text("还没有持久提交的结果。运行中、被拒绝或效果未知的调用不会补造结果。").foregroundStyle(.secondary)
              }
            } else if tab == "schema" {
              if let schema = detail.schema {
                code(schema)
              } else {
                Text("这个调用没有记录工具定义。").foregroundStyle(.secondary)
              }
            } else {
              if let start = detail.started {
                LabeledContent(
                  "开始时间",
                  value: Date(timeIntervalSince1970: Double(start)).formatted(
                    date: .abbreviated, time: .standard))
              }
              if let wait = detail.waitMs { LabeledContent("授权等待", value: toolDuration(wait)) }
              if let execution = detail.executionMs {
                LabeledContent("工具调用耗时", value: toolDuration(execution))
              }
              if let commit = detail.committedMs {
                LabeledContent("执行至结果确认", value: toolDuration(commit))
              }
              if detail.executionMs == nil { Text("没有可用的执行时长记录。").foregroundStyle(.secondary) }
              Text("授权等待、工具返回与结果提交分别计量。运行中的调用不显示推测的完成时长。").font(.caption).foregroundStyle(
                .secondary)
            }
          }.font(.callout).frame(maxWidth: .infinity, alignment: .leading).padding(.vertical, 4)
        }
        Divider()
        HStack {
          Button("聊天原文") {
            let sequence = detail.resultSequence ?? detail.sourceSequence
            if let onChat { onChat(sequence) } else { store.jumpToToolSource(sequence) }
          }
            .accessibilityIdentifier("tools.source")
          Spacer()
          if !detail.historical {
            Button("关联轨迹") {
              if let onTrace { onTrace(detail.run, detail.callID); return }
              store.trajectoryRunFocus = detail.run
              store.trajectoryCallFocus = detail.callID
              store.route = "chat"
              store.conversationPage = "trajectory"
            }
          }
        }
        if let id = detail.fileReceiptID {
          Button("查看文件差异与回退…") {
            let selection = FileReceiptSelection(id: id, chat: chat, origin: .tools)
            if initialChat.isEmpty { store.selectedReceipt = selection } else { embeddedReceipt = selection }
          }
            .accessibilityIdentifier("tools.fileReceipt")
        }
      }.padding(20)
    } else {
      ContentUnavailableView(
        "选择一个调用", systemImage: "sidebar.right", description: Text("在这里查看参数、结果、授权与时间记录。"))
    }
  }
  private func code(_ value: String) -> some View {
    ToolPayloadView(value: value)
  }
  private func grantLabel(_ status: String) -> String {
    switch status {
    case "awaiting": return "等待决定"
    case "issued_once": return "仅限这一次调用"
    case "denied": return "已拒绝"
    case "expired": return "已结束"
    case "interrupted": return "已中断"
    default: return status
    }
  }
  private func resetAndLoad() {
    records = []
    selectedID = nil
    detail = nil
    cursor = 0
    more = false
    initialized = false
    followsTail = true
    loading = false
    requestID = UUID()
    load()
  }
  private func load(older: Bool = false) {
    guard !chat.isEmpty, !loading else { return }
    loading = true
    let token = UUID()
    requestID = token
    let owner = chat
    store.readLocalView(
      [
        "op": "tools.list", "chat": owner, "before": older ? cursor : 0, "query": query,
        "stage": stage,
      ], as: TrajectoryPage<ToolRecord>.self
    ) { reply in
      guard token == requestID, owner == chat else { return }
      loading = false
      guard reply.ok, let page = reply.data else {
        error = reply.error ?? "工具记录暂不可读。"
        return
      }
      let incoming = Set(page.items.map(\.id))
      records = records.filter { !incoming.contains($0.id) } + page.items
      if older || !initialized {
        cursor = page.nextCursor
        more = page.more
      }
      initialized = true
      error = ""
    }
  }
  private func loadDetail() {
    guard let id = selectedID else {
      detail = nil
      return
    }
    let owner = chat
    let token = UUID()
    detailRequestID = token
    store.readLocalView(["op": "tools.detail", "chat": owner, "id": id], as: ToolRecord.self) {
      reply in
      guard token == detailRequestID, owner == chat, selectedID == id else { return }
      if reply.ok {
        detail = reply.data
      } else {
        detail = nil
        error = reply.error ?? "调用详情暂不可读。"
      }
    }
  }
  private func locate() {
    guard let sequence = store.toolFocusSequence, !chat.isEmpty else { return }
    let owner = chat
    store.readLocalView(
      ["op": "tools.locate", "chat": owner, "sequence": sequence],
      as: TrajectoryPage<ToolRecord>.self
    ) { reply in
      guard owner == chat, store.toolFocusSequence == sequence else { return }
      if reply.ok, let page = reply.data, let row = page.items.first {
        if !records.contains(where: { $0.id == row.id }) { records.append(row) }
        followsTail = false
        selectedID = row.id
        loadDetail()
      } else {
        error = "这个消息没有可定位的工具调用记录。"
      }
    }
  }
}

private struct ToolPayloadView: View {
  let value: String
  @State private var page = 0
  private let pageSize = 20000
  private var range: Range<String.Index> {
    let start =
      value.index(value.startIndex, offsetBy: page * pageSize, limitedBy: value.endIndex)
      ?? value.endIndex
    let end = value.index(start, offsetBy: pageSize, limitedBy: value.endIndex) ?? value.endIndex
    return start..<end
  }
  var body: some View {
    let current = range
    VStack(alignment: .leading, spacing: 8) {
      HStack {
        if page > 0 || current.upperBound < value.endIndex {
          Text("原文第 \(page+1) 段").foregroundStyle(.secondary)
          Button("上一段") { page = max(0, page - 1) }.disabled(page == 0)
          Button("下一段") { page += 1 }.disabled(current.upperBound == value.endIndex)
        }
        Spacer()
        Button {
          NSPasteboard.general.clearContents()
          NSPasteboard.general.setString(value, forType: .string)
        } label: {
          Label("复制完整原文", systemImage: "doc.on.doc")
        }.buttonStyle(.borderless)
      }.font(.caption)
      Text(String(value[current])).font(.system(.callout, design: .monospaced)).textSelection(
        .enabled
      ).frame(maxWidth: .infinity, alignment: .leading)
    }.padding(12).background(Color.primary.opacity(0.035), in: RoundedRectangle(cornerRadius: 8))
      .onChange(of: value) { _, _ in page = 0 }
  }
}
