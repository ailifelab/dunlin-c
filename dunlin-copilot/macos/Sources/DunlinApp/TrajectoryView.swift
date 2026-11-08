import AppKit
import CDesktop
import Combine
import SwiftUI

struct TrajectorySegment: Decodable, Identifiable {
  let cursor: Int64
  let id: String
  let run: String
  let model: String
  let `protocol`: String
  let started: Int64
  let duration: Int64?
  let firstText: Int64?
  let sourceStart: Int64
  let sourceEnd: Int64?
  let status: String
  let partial: Bool
}
struct TrajectoryEvent: Decodable, Identifiable {
  let id: Int64
  let kind: String
  let ms: Int64
  let attempt: Int
  let body: JSONValue
  let partial: Bool
  var title: String {
    switch kind {
    case "turn.start": return "回合开始"
    case "turn.end": return "回合返回"
    case "model.request": return "模型请求 · 输入"
    case "model.response": return "模型响应 · 输出"
    case "model.metrics": return "模型用量与耗时"
    case "model.visible_text": return "流式可见文本"
    case "model.first_visible_text": return "收到本次请求的首段文本"
    case "model.request.end": return "本次提议检查结束"
    case "model.proposal_retry", "tool.required_retry": return "提议未通过，准备重试"
    case "model.progress": return "等待模型响应"
    case "approval.request": return "等待工具授权"
    case "approval.decision": return "授权决定已记录"
    case "tool.pre_execute": return "工具执行前检查通过"
    case "tool.start": return "开始调用工具"
    case "tool.post_execute": return "工具已返回，待提交"
    case "tool.finalized": return "工具结果已持久提交"
    case "tool.result": return "工具结果通知"
    case "tool.outcome_unknown": return "外部效果未知"
    case "source.committed": return "聊天原文已提交"
    case "context.compacting": return "正在压缩较早回合"
    case "context.model.request": return "摘要模型请求 · 输入"
    case "context.model.response": return "摘要模型响应 · 输出"
    case "context.model.metrics": return "摘要模型用量与耗时"
    case "context.model.end": return "摘要请求结束"
    case "context.compacted": return "派生上下文已应用"
    case "knowledge.context": return "知识引用已选取"
    case "admission.result": return "回合准入结果"
    case "file.operation.request": return "用户确认文件回退"
    case "file.operation.result": return "文件回退结果"
    case "file.operation.rejected": return "文件回退检查未通过"
    case "recovery.abandoned": return "用户结束回合，保留未决记录"
    default: return kind
    }
  }
  var color: Color {
    if kind.contains("unknown") || kind.contains("error") || kind.contains("retry") {
      return .orange
    }
    if kind.hasPrefix("approval") { return .purple }
    if kind.hasPrefix("tool") { return .green }
    if kind.hasPrefix("context") { return .teal }
    return .accentColor
  }
  var fields: [String: JSONValue] {
    if case .object(let value) = body { return value }
    return [:]
  }
  var sourceSequence: Int? {
    if case .integer(let n) = fields["sequence"] { return Int(n) }
    return nil
  }
}
struct TrajectoryPage<Item: Decodable>: Decodable {
  let items: [Item]
  let more: Bool
  let nextCursor: Int64
}
struct TrajectoryStatus: Decodable {
  let available: Bool
  let enabled: Bool?
  let capture: Bool?
  let quota: Int64?
  let payloadBytes: Int64?
  let segments: Int64?
}
private struct TraceInterval: Identifiable {
  let id: Int64
  let title: String
  let start: Int64
  var end: Int64?
  let color: Color
}
private struct TraceIntervalBar: View {
  let phase: TraceInterval
  let scale: Int64
  var body: some View {
    GeometryReader { geometry in
      let left: CGFloat = CGFloat(phase.start) / CGFloat(scale) * geometry.size.width
      let width: CGFloat =
        phase.end.map {
          max(
            CGFloat(2),
            CGFloat(max(Int64(0), $0 - phase.start)) / CGFloat(scale) * geometry.size.width)
        } ?? CGFloat(3)
      let clipped: CGFloat = min(width, max(CGFloat(3), geometry.size.width - left))
      let offset: CGFloat = min(left, geometry.size.width - 3)
      let opacity: Double = phase.end == nil ? 0.4 : 0.8
      ZStack(alignment: .leading) {
        Capsule().fill(Color.primary.opacity(0.04))
        Capsule().fill(phase.color.opacity(opacity)).frame(width: clipped).offset(x: offset)
      }
    }
  }
}
extension WorkspaceStore {
  func readTrajectory<T: Decodable>(
    _ body: [String: Any], as: T.Type, completion: @escaping (WorkspaceReply<T>) -> Void
  ) { readLocalView(body, as: T.self, completion: completion) }
  func readLocalView<T: Decodable>(
    _ body: [String: Any], as: T.Type, completion: @escaping (WorkspaceReply<T>) -> Void
  ) {
    guard let data = try? JSONSerialization.data(withJSONObject: body),
      let raw = String(data: data, encoding: .utf8)
    else { return }
    let path = directory.path
    let epoch = identityEpoch
    DispatchQueue.global(qos: .utility).async {
      let result =
        self.decode(dc_read_view(path, raw), as: WorkspaceReply<T>.self)
        ?? WorkspaceReply(ok: false, error: "本地记录暂不可读，原始数据仍保留。", data: nil)
      DispatchQueue.main.async { if self.identityEpoch == epoch { completion(result) } }
    }
  }
}
private func traceStatus(_ value: String) -> String {
  switch value {
  case "recording": return "运行中"
  case "completed": return "已完成"
  case "interrupted": return "进程中断"
  case "unknown": return "效果未知"
  case "abandoned_unknown": return "已结束 · 效果未知"
  case "abandoned": return "已由用户结束"
  case "cancelled": return "已停止"
  case "denied": return "授权拒绝"
  case "failed": return "失败"
  case "budget": return "预算用尽"
  case "timeout": return "超时"
  case "busy": return "准入未通过"
  default: return value
  }
}
private func milliseconds(_ value: Int64) -> String {
  value < 1000 ? "\(value) ms" : String(format: "%.2f s", Double(value) / 1000)
}
private func trajectoryError(_ value: String?) -> String {
  switch value {
  case "trace_cache_unavailable": return "轨迹缓存暂不可用，聊天原文仍保留。"
  case "chat_not_found": return "当前聊天已移除或暂不可读。"
  case "trace_not_found": return "此轨迹已清理或超过保留限额。"
  case "trace_storage_error": return "轨迹缓存读取未完成，请刷新后重试。"
  case "invalid_trace_query", "invalid_trace_settings": return "轨迹查询或配额参数无效。"
  default: return value ?? "轨迹操作未完成"
  }
}

struct TrajectoryView: View {
  var initialChat = ""
  var embedded = false
  @EnvironmentObject var store: WorkspaceStore
  @Environment(\.dismiss) private var dismiss
  @State private var chat = ""
  @State private var segments: [TrajectorySegment] = []
  @State private var segmentID: String?
  @State private var segmentMore = false
  @State private var segmentCursor: Int64 = 0
  @State private var events: [TrajectoryEvent] = []
  @State private var selectedEvent: Int64?
  @State private var eventMore = false
  @State private var eventCursor: Int64 = 0
  @State private var kind = ""
  @State private var query = ""
  @State private var error = ""
  @State private var requestID = UUID()
  @State private var segmentRequestID = UUID()
  @State private var exportValue: TraceExport?
  private let tick = Timer.publish(every: 1.5, on: .main, in: .common).autoconnect()
  private var segment: TrajectorySegment? { segments.first { $0.id == segmentID } }
  private var detail: TrajectoryEvent? { events.first { $0.id == selectedEvent } }
  private var intervals: [TraceInterval] {
    var result: [TraceInterval] = []
    var active: [String: Int] = [:]
    for e in events.sorted(by: { $0.id < $1.id }) {
      let isModel = e.kind == "model.request" || e.kind == "context.model.request"
      let isApproval = e.kind == "approval.request"
      let isTool = e.kind == "tool.start"
      let isEnd =
        e.kind == "model.request.end" || e.kind == "context.model.end"
        || e.kind == "approval.decision" || e.kind == "tool.finalized"
        || e.kind == "tool.outcome_unknown"
      var key = ""
      if isModel || e.kind == "model.request.end" || e.kind == "context.model.end" {
        key = "model:\(e.attempt)"
      } else if isApproval || e.kind == "approval.decision" {
        key = "approval:" + (e.fields["grant"]?.formatted ?? "")
      } else if isTool || e.kind == "tool.finalized" || e.kind == "tool.outcome_unknown" {
        key = "tool:" + e.body.formatted
      }
      if isModel || isApproval || isTool {
        active[key] = result.count
        result.append(
          TraceInterval(
            id: e.id, title: isModel ? "模型 \(e.attempt)" : isApproval ? "授权等待" : "工具提交",
            start: e.ms, end: nil, color: e.color))
      } else if isEnd, let i = active.removeValue(forKey: key) {
        result[i].end = e.ms
      }
    }
    return result
  }
  var body: some View {
    VStack(spacing: 0) {
      HStack(spacing: 16) {
        VStack(alignment: .leading, spacing: 5) {
          Text("运行轨迹").font(.title2.weight(.semibold))
          Text(store.selectedChat?.title ?? "聊天").font(.callout).foregroundStyle(.secondary)
        }
        Spacer()
        if store.busy {
          ProgressView().controlSize(.small)
          Text("实时观察").font(.caption).foregroundStyle(.secondary)
        }
        if store.trajectoryRunFocus != nil {
          Button("显示全部回合") {
            store.trajectoryRunFocus = nil
            store.trajectoryCallFocus = nil
            query = ""
            segments = []
            segmentID = nil
            reloadSegments()
          }
        }
        Button {
          reloadSegments()
        } label: {
          Label("刷新", systemImage: "arrow.clockwise")
        }.accessibilityIdentifier("trajectory.refresh")
        Button("导出…") { export() }.disabled(segmentID == nil).accessibilityIdentifier(
          "trajectory.export")
        Menu {
          Button("清理此聊天轨迹…", role: .destructive) { clear() }.disabled(store.busy)
        } label: {
          Image(systemName: "ellipsis")
        }.menuIndicator(.hidden).help("轨迹管理")
        if !embedded { Button("完成") { dismiss() }.keyboardShortcut(.cancelAction) }
      }.padding(24)
      Divider()
      HSplitView {
        VStack(alignment: .leading, spacing: 0) {
          Text("回合与恢复记录").font(.headline).padding(16)
          List(selection: $segmentID) {
            ForEach(segments) { s in
              VStack(alignment: .leading, spacing: 6) {
                HStack {
                  Text(s.model).fontWeight(.medium).lineLimit(1)
                  Spacer()
                  if s.partial {
                    Image(systemName: "exclamationmark.circle").foregroundStyle(.orange).help(
                      "此轨迹存在省略或中断")
                  }
                }
                HStack {
                  Text(traceStatus(s.status))
                  Spacer()
                  if let duration = s.duration { Text(milliseconds(duration)) }
                }.font(.caption).foregroundStyle(.secondary)
                Text(
                  Date(timeIntervalSince1970: Double(s.started) / 1000),
                  format: .dateTime.month().day().hour().minute().second()
                ).font(.caption2).foregroundStyle(.secondary)
                Text("回合 \(s.run.prefix(8)) · \(s.protocol)").font(.caption2.monospaced())
                  .foregroundStyle(.tertiary)
              }.padding(.vertical, 7).tag(s.id)
            }
          }.listStyle(.sidebar).accessibilityIdentifier("trajectory.segments")
          if segmentMore { Button("更早的回合") { reloadSegments(older: true) }.padding(12) }
          if segments.isEmpty {
            Text("此聊天还没有可用轨迹。关闭记录期间的运行不会补造轨迹。").font(.caption).foregroundStyle(.secondary).padding(
              16)
          }
        }.frame(minWidth: 220, idealWidth: 245, maxWidth: 300).background(.regularMaterial)
        VStack(alignment: .leading, spacing: 14) {
          if let segment {
            HStack {
              Text(traceStatus(segment.status)).font(.headline)
              Spacer()
              if let first = segment.firstText {
                Text("开始后 \(milliseconds(first)) 收到首段文本").font(.caption).foregroundStyle(.secondary)
              }
            }
            if segment.partial {
              Label("轨迹存在缺口；不能据此判定外部效果未发生。", systemImage: "exclamationmark.triangle").font(.caption)
                .foregroundStyle(.orange)
            }
            Text("模型响应结束、工具结果提交和回合结束分别记录。缺失用量不会显示为 0。").font(.caption).foregroundStyle(.secondary)
            if !intervals.isEmpty { timeline }
            HStack {
              Picker("事件", selection: $kind) {
                Text("全部").tag("")
                Text("模型").tag("model.")
                Text("工具").tag("tool.")
                Text("授权").tag("approval.")
                Text("上下文").tag("context.")
                Text("文件回退").tag("file.")
                Text("原文").tag("source.")
              }.labelsHidden().frame(width: 120)
              TextField("搜索轨迹内容", text: $query).textFieldStyle(.roundedBorder).onSubmit {
                reloadEvents()
              }.accessibilityIdentifier("trajectory.search")
              Button("查找") { reloadEvents() }
            }
            ScrollViewReader { proxy in
              ScrollView {
                LazyVStack(alignment: .leading, spacing: 2) {
                  if eventMore {
                    Button("加载更早的事件") { reloadEvents(older: true) }.frame(maxWidth: .infinity)
                      .padding(8)
                  }
                  ForEach(events.sorted { $0.id < $1.id }) { e in
                    Button {
                      selectedEvent = e.id
                    } label: {
                      HStack(alignment: .top, spacing: 12) {
                        VStack(spacing: 5) {
                          Circle().fill(e.color).frame(width: 8, height: 8)
                          Rectangle().fill(e.color.opacity(0.18)).frame(width: 1, height: 29)
                        }.padding(.top, 5)
                        VStack(alignment: .leading, spacing: 5) {
                          Text(e.title).font(.callout.weight(.medium))
                          HStack {
                            Text("+" + milliseconds(e.ms))
                            if e.attempt > 0 { Text("尝试 \(e.attempt)") }
                            if e.partial { Text("部分记录").foregroundStyle(.orange) }
                          }.font(.caption).foregroundStyle(.secondary)
                        }
                        Spacer()
                        if e.kind == "model.metrics" || e.kind == "context.model.metrics" {
                          Image(systemName: "chart.bar.xaxis").foregroundStyle(.secondary)
                        }
                      }.padding(10).frame(maxWidth: .infinity, alignment: .leading).background(
                        selectedEvent == e.id ? Color.accentColor.opacity(0.10) : .clear,
                        in: RoundedRectangle(cornerRadius: 8))
                    }.buttonStyle(.plain).id(e.id).accessibilityIdentifier(
                      "trajectory.event.\(e.id)")
                  }
                }
              }.onChange(of: events.last?.id) { _, value in
                if let value, selectedEvent == nil { proxy.scrollTo(value, anchor: .bottom) }
              }
            }
          } else {
            ContentUnavailableView(
              "选择一个回合", systemImage: "point.3.connected.trianglepath.dotted",
              description: Text("查看模型请求、工具执行和授权的先后关系。"))
          }
        }.padding(20).frame(minWidth: 380, idealWidth: 470)
        inspector.frame(minWidth: 280, idealWidth: 340, maxWidth: 440)
      }
      if !error.isEmpty {
        Text(error).foregroundStyle(.orange).font(.callout).textSelection(.enabled).padding(14)
          .frame(maxWidth: .infinity, alignment: .leading)
      }
    }.frame(width: embedded ? nil : 1150, height: embedded ? nil : 780).onAppear {
      chat = initialChat.isEmpty ? (store.selected ?? ""):initialChat
      if let call = store.trajectoryCallFocus { query = call }
      reloadSegments()
    }
    .onChange(of: segmentID) { _, _ in
      selectedEvent = nil
      reloadEvents()
    }
    .onChange(of: kind) { _, _ in
      selectedEvent = nil
      reloadEvents()
    }
    .onChange(of: store.identityEpoch) { _, _ in if !embedded { dismiss() } }
    .onChange(of: store.selected) { _, value in
      chat = initialChat.isEmpty ? (value ?? ""):initialChat
      segments = []
      segmentID = nil
      events = []
      reloadSegments()
    }
    .onReceive(tick) { _ in if store.busy { reloadSegments() } }
    .sheet(item: $exportValue) { TraceExportView(value: $0) }
  }
  private var inspector: some View {
    VStack(alignment: .leading, spacing: 16) {
      Text("事件详情").font(.headline)
      if let detail {
        Text(detail.title).font(.callout.weight(.medium))
        LabeledContent("距本次运行开始", value: milliseconds(detail.ms)).font(.caption)
        if detail.attempt > 0 { LabeledContent("模型尝试", value: "\(detail.attempt)").font(.caption) }
        if detail.kind.contains("metrics") {
          Text("耗时包含请求至响应结束。字节数不含 HTTP 头；不可用的 token 统计保持缺失。首段文本延迟不是所有模型的首 token 延迟。").font(.caption)
            .foregroundStyle(.secondary)
        }
        if let sequence = detail.sourceSequence, let content = detail.fields["content"],
          case .string = content
        {
          Button("定位聊天原文 \(sequence+1)") {
            store.jumpToToolSource(sequence)
            if !embedded { dismiss() }
          }.accessibilityIdentifier("trajectory.source")
        }
        ScrollView {
          Text(detail.body.formatted).font(.system(size: 11, design: .monospaced)).textSelection(
            .enabled
          ).frame(maxWidth: .infinity, alignment: .leading)
        }
        if detail.partial { Text("详情因记录策略或本地限额有所省略。").font(.caption).foregroundStyle(.orange) }
      } else {
        Text("选择事件查看输入、输出、时序与来源。").font(.callout).foregroundStyle(.secondary)
        Spacer()
      }
    }.padding(20).background(Color(nsColor: .controlBackgroundColor))
  }
  private var timeline: some View {
    let scale = max(1, segment?.duration ?? events.map(\.ms).max() ?? 1)
    return VStack(alignment: .leading, spacing: 7) {
      HStack {
        Text("当前已加载事件的时序").font(.caption.weight(.medium))
        Spacer()
        Text(milliseconds(scale)).font(.caption.monospacedDigit()).foregroundStyle(.secondary)
      }
      ScrollView(.vertical) {
        VStack(spacing: 7) {
          ForEach(intervals) { phase in
            HStack(spacing: 10) {
              Text(phase.title).font(.caption).frame(width: 68, alignment: .leading)
              TraceIntervalBar(phase: phase, scale: scale).frame(height: 9).help(
                phase.end.map { milliseconds($0 - phase.start) } ?? "当前筛选或分页中没有结束记录；没有推算完成时间")
              Text(phase.end.map { milliseconds($0 - phase.start) } ?? "结束未显示").font(
                .caption2.monospacedDigit()
              ).foregroundStyle(.secondary).frame(width: 57, alignment: .trailing)
            }
          }
        }
      }.frame(maxHeight: 100)
    }.padding(12).background(Color.primary.opacity(0.025), in: RoundedRectangle(cornerRadius: 10))
  }
  private func reloadSegments(older: Bool = false) {
    guard !chat.isEmpty else { return }
    let token = UUID()
    segmentRequestID = token
    var body: [String: Any] = ["op": "trace.segments", "chat": chat]
    if let run = store.trajectoryRunFocus { body["run"] = run }
    if older { body["before"] = segmentCursor }
    store.readTrajectory(body, as: TrajectoryPage<TrajectorySegment>.self) { r in
      guard segmentRequestID == token else { return }
      if let page = r.data, r.ok {
        segments = older ? segments + page.items : page.items
        segmentMore = page.more
        segmentCursor = page.nextCursor
        error = ""
        if segmentID == nil {
          segmentID =
            segments.first(where: { $0.run == store.trajectoryRunFocus })?.id ?? segments.first?.id
        } else if !segments.contains(where: { $0.id == segmentID }) {
          segmentID = segments.first?.id
        }
        if store.busy { reloadEvents() }
      } else {
        error = trajectoryError(r.error)
      }
    }
  }
  private func reloadEvents(older: Bool = false) {
    guard let segmentID else {
      events = []
      return
    }
    let token = UUID()
    requestID = token
    var body: [String: Any] = [
      "op": "trace.events", "chat": chat, "segment": segmentID, "kind": kind, "query": query,
    ]
    if older { body["before"] = eventCursor }
    store.readTrajectory(body, as: TrajectoryPage<TrajectoryEvent>.self) { r in
      guard requestID == token, self.segmentID == segmentID else { return }
      if let page = r.data, r.ok {
        events = older ? page.items + events : page.items
        eventMore = page.more
        eventCursor = page.nextCursor
        error = ""
      } else {
        error = trajectoryError(r.error)
      }
    }
  }
  private func export() {
    guard let segmentID else { return }
    store.readTrajectory(
      ["op": "trace.export", "chat": chat, "segment": segmentID], as: JSONValue.self
    ) { r in
      if let value = r.data, r.ok {
        exportValue = TraceExport(text: value.formatted)
      } else {
        error = trajectoryError(r.error)
      }
    }
  }
  private func clear() {
    let alert = NSAlert()
    alert.messageText = "清理此聊天的运行轨迹？"
    alert.informativeText = "已结束的轨迹片段将被永久清理，不能补回当时的请求和时序。聊天原文、项目文件与授权审计保留。"
    alert.addButton(withTitle: "清理轨迹")
    alert.addButton(withTitle: "取消")
    alert.buttons[0].keyEquivalent = ""
    alert.window.defaultButtonCell = nil
    guard alert.runModal() == .alertFirstButtonReturn else { return }
    store.workspaceRequest(["op": "trace.clear", "chat": chat], as: NoData.self) { r in
      if r.ok {
        segmentID = nil
        events = []
        reloadSegments()
      } else {
        error = trajectoryError(r.error)
      }
    }
  }
}
struct TraceExport: Identifiable {
  let id = UUID()
  let text: String
}
struct TraceExportView: View {
  let value: TraceExport
  @Environment(\.dismiss) private var dismiss
  @State private var error = ""
  var body: some View {
    VStack(alignment: .leading, spacing: 16) {
      Text("导出轨迹预览").font(.title2.weight(.semibold))
      Text("已移除模型密钥及结构化敏感字段。用户文本、文件内容和命令输出仍可能包含私人信息，请检查后保存或分享。此导出只包含所选运行片段。").font(.callout)
        .foregroundStyle(.secondary)
      ScrollView {
        Text(value.text).font(.system(size: 12, design: .monospaced)).textSelection(.enabled).frame(
          maxWidth: .infinity, alignment: .leading)
      }
      HStack {
        Text(error).foregroundStyle(.red)
        Spacer()
        Button("取消") { dismiss() }
        Button("保存 JSON…") { save() }.buttonStyle(.borderedProminent)
      }
    }.padding(28).frame(width: 820, height: 650)
  }
  private func save() {
    let panel = NSSavePanel()
    panel.nameFieldStringValue = "dunlin-trajectory.json"
    guard panel.runModal() == .OK, let url = panel.url else { return }
    do {
      try Data(value.text.utf8).write(to: url, options: .atomic)
      dismiss()
    } catch { self.error = error.localizedDescription }
  }
}
struct TrajectorySettingsView: View {
  @EnvironmentObject var store: WorkspaceStore
  @State private var status: TrajectoryStatus?
  @State private var quota = 64
  @State private var error = ""
  var body: some View {
    Form {
      Section("本地运行轨迹") {
        if let status, status.available {
          Toggle(
            "记录模型与工具运行轨迹",
            isOn: Binding(get: { status.enabled ?? false }, set: { update(["enabled": $0]) })
          ).accessibilityIdentifier("trajectory.enabled")
          Toggle(
            "保留脱敏后的输入与输出详情",
            isOn: Binding(get: { status.capture ?? false }, set: { update(["capture": $0]) })
          ).accessibilityIdentifier("trajectory.capture")
          LabeledContent("记录方式", value: "本地 · 不自动上传")
          LabeledContent("轨迹片段", value: "\(status.segments ?? 0)")
          LabeledContent(
            "缓存内容与记录开销",
            value: ByteCountFormatter.string(
              fromByteCount: status.payloadBytes ?? 0, countStyle: .file))
          HStack {
            TextField("内容配额（MiB）", value: $quota, format: .number)
            Button("应用") { update(["quota": Int64(quota) * 1_048_576]) }.disabled(
              quota < 1 || quota > 4096)
          }
          Text("仅清理已结束的较早轨迹，保留聊天原文；活动轨迹达到限额会标记部分覆盖。此配额计算记录内容与行开销，SQLite 页面、日志和临时空间另计。").font(
            .caption
          ).foregroundStyle(.secondary)
          Text("关闭记录或详情后只影响后续运行，已有记录保留。模型返回不表示任务完成；中断和未知外部效果会分别保留。").font(.caption).foregroundStyle(
            .secondary)
        } else {
          Text("轨迹缓存暂不可用，聊天原文与执行授权仍由各自模块保存。").foregroundStyle(.secondary)
        }
        if !error.isEmpty { Text(error).foregroundStyle(.orange) }
      }
    }.formStyle(.grouped).onAppear {
      store.readTrajectory(["op": "trace.status"], as: TrajectoryStatus.self) { r in
        status = r.data
        if let bytes = r.data?.quota { quota = Int(bytes / 1_048_576) }
      }
    }
  }
  private func update(_ changes: [String: Any]) {
    var body = changes
    body["op"] = "trace.config"
    store.workspaceRequest(body, as: TrajectoryStatus.self) { r in
      if r.ok {
        status = r.data
        error = ""
      } else {
        error = trajectoryError(r.error)
      }
    }
  }
}
