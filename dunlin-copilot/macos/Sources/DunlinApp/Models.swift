import AppKit
import CDesktop
import Security
import SwiftUI

struct ModelProfile: Codable, Identifiable, Hashable {
  var id = UUID().uuidString
  var name = "本地模型"
  var endpoint = "http://127.0.0.1:8080/v1"
  var model = "local-model"
  var protocolName = "chat"
  var maxOutput = 4096
  var keyEnvironment: String?
  var enableThinking: Bool?
  var systemPrompt: String?
  var chatTemplate: String?
  var authentication: String?
  var credentialRevision: Int?
  var credentialID: String?
  var providerID: String?
  var reasoningSupported: Bool?
  var reasoningCapabilities: [String]?
  var reasoningEffort: String?
  var temperature: Double?
  var inputModalities: [String]?
  var keychainID: String { credentialID ?? providerID ?? id }
  var contextLimit: Int?
  var deploymentLimit: Int?
  var outputLimit: Int?
  var advertisedOutputLimit: Int?
  var effectiveOutputLimit: Int {
    [outputLimit, advertisedOutputLimit].compactMap { $0 }.filter { $0 > 0 }.min() ?? 65536
  }
  var contextBudget: Int?
  var contextSource: String?
  var contextReserve: Int?
  var compactionStrategy: String?
  private var contextCapabilities: dc_context_capabilities {
    dc_context_capabilities(
      model_limit: UInt64(max(0, contextLimit ?? 0)),
      deployment_limit: UInt64(max(0, deploymentLimit ?? 0)),
      selected_limit: UInt64(max(0, contextBudget ?? 0)), client_limit: 4_194_304,
      output_limit: UInt64(max(0, effectiveOutputLimit)),
      requested_output: UInt64(max(0, maxOutput)),
      overhead_reserve: UInt64(max(0, contextReserve ?? 512)))
  }
  var contextRange: dc_context_range? {
    var caps = contextCapabilities
    var result = dc_context_range()
    return dc_context_budget(&caps, &result) == DC_CONTEXT_VALID ? result : nil
  }
  var contextEditorRange: dc_context_range? {
    guard let bounds = ModelContextBudget.limits(contextCapabilities) else { return nil }
    var caps = contextCapabilities
    let requested = contextBudget.flatMap { $0 == 0 ? nil : $0 } ?? Int(bounds.recommended)
    caps.selected_limit = UInt64(ModelContextBudget.choice(requested, within: bounds))
    var result = dc_context_range()
    return dc_context_budget(&caps, &result) == DC_CONTEXT_VALID ? result : nil
  }
  @discardableResult mutating func setEditorContextBudget(_ value: Int) -> Bool {
    guard let bounds = ModelContextBudget.limits(contextCapabilities) else { return false }
    contextBudget = ModelContextBudget.choice(value, within: bounds)
    return true
  }
}
struct ContextPreferences: Codable, Equatable {
  var strategy = "semantic"
  var automatic = true
  var threshold = 0.8
  var retainTurns = 2
  var summaryOutput = 1024
}
struct KnowledgePreferences: Codable, Equatable {
  var memory = false
  var rag = false
}
struct DesktopSettings: Codable {
  var profileID: String
  var theme: String
  var showReasoning: Bool
  var context: ContextPreferences?
  var defaultProfileID: String?
  var knowledge: KnowledgePreferences?
  var general: GeneralPreferences?
  var media: MediaPreferences?
  var settingsSection:String?
}
struct Chat: Codable, Identifiable, Hashable {
  let id: String
  let title: String
  let workspace: String
  let workspaceDevice: String?
  let workspaceInode: String?
  let workspaceRevision: Int64?
  let workspaceAvailable: Bool?
  let workspaceAvailability: String?
  let workspaceOrigin: String?
  let project: String
  let archived: Bool
  let pinned: Bool
  let active: Bool
  let unsupportedCheckpoint: Bool?
  let model: String
  let updated: Int64?
  let lastStatus: String?
  var statusLabel: String {
    if unsupportedCheckpoint == true { return "不支持的存档格式" }
    if active { return "未完成 · 等待恢复" }
    switch lastStatus {
    case "completed": return "已完成"
    case "cancelled": return "已停止"
    case "failed", "timeout", "denied": return "未完成"
    case "budget_exhausted": return "预算已用尽"
    case "abandoned_unknown": return "已结束 · 结果未知"
    case "abandoned": return "已结束"
    default: return "尚无已完成回合"
    }
  }
}
struct ProjectRecord: Codable, Identifiable, Hashable {
  var id = UUID().uuidString
  var name = ""
  var path = ""
  var instructions = ""
  var model = ""
  var policy = "ask"
  var context: [String] = []
  var expanded = true
  var revision = 0
  var device = ""
  var inode = ""
  var bookmark = ""
  var chatCount = 0
  var available = true
  var availability = ""
}
struct ProjectFile: Codable, Identifiable {
  var id: String { path }
  let path: String
  let name: String
  let directory: Bool
  let size: Int64
}
struct ProjectDocument: Codable, Identifiable {
  var id: String { path }
  let path: String
  let sha256: String
  let text: String
}
struct ProjectReferenceState: Decodable, Identifiable {
  var id: String { path }
  let path: String
  let baselineSHA: String?
  let currentSHA: String?
  let state: String
  var message: String {
    switch state {
    case "current": return "已固定 · 当前版本可访问"
    case "changed": return "文件已更新 · 使用前请重新预览"
    case "unavailable": return "文件不可访问或格式不支持"
    default: return "旧版引用 · 尚未确认文件版本"
    }
  }
}
struct WorkspaceReply<T: Decodable>: Decodable {
  let ok: Bool
  let error: String?
  let data: T?
  let diagnostic: ConfigDiagnostic?
  init(ok: Bool, error: String?, data: T?, diagnostic: ConfigDiagnostic? = nil) {
    self.ok = ok
    self.error = error
    self.data = data
    self.diagnostic = diagnostic
  }
}
struct ConfigDiagnostic: Decodable {
  let code: String
  let path: String?
  let line: Int?
  let column: Int?
}
struct ChatMetadata: Decodable {
  let draft: String
  let model: String
}
struct NoData: Decodable {}
struct DesktopIdentity: Codable, Identifiable, Hashable {
  let id: String
  let name: String
  let kind: String
  let members: [String]
  let hidden: Bool
  let selected: Bool
  let epoch: Int64
  let directory: String
}
struct Message: Codable, Identifiable {
  private var fallbackID = UUID()
  var id: String { sequence.map { "message:\($0)" } ?? fallbackID.uuidString }
  let sequence: Int?
  let sourceHash: String?
  let observation: String?
  let role: String
  let content: String
  var reasoning: String? = nil
  var reasoningKind: String? = nil
  var reasoningUnavailable: Bool? = nil
  var modelName: String? = nil
  var attachment: ChatAttachment? = nil
  var media: [String:String]? = nil
  enum CodingKeys: String, CodingKey {
    case sequence, sourceHash, observation, role, content, reasoning, reasoningKind, reasoningUnavailable, modelName, attachment, media
  }
}
struct ChatCommit: Decodable {
  let chat: String
  let run: String
  let through: Int
  let settlesModel: Bool
}
struct SearchHit: Codable, Identifiable {
  var id: String { chat + ":" + String(sequence) }
  let chat: String
  let sequence: Int
  let text: String
}

struct FileChange: Identifiable, Codable {
  var id = UUID()
  var path: String
  var before: String
  var after: String
  var relativePath: String? = nil
  var existed: Bool
}
struct GrantRecord: Identifiable {
  var chat: String
  var id = UUID()
  var tool: String
  var arguments: String
  var allowed: Bool
  var date = Date()
  var recordID: Int64?
  var status = "issued_once"
  var waitMs = 0
  var decided: Int64 = 0
  var ended: Int64 = 0
}
struct StoredGrant: Decodable {
  let id: Int64
  let chat: String
  let tool: String
  let arguments: String
  let argsHash: String
  let allowed: Bool
  let status: String
  let started: Int64
  let decided: Int64
  let ended: Int64
  let waitMs: Int
}

struct SkillResource: Codable, Hashable {
  let path: String
  let digest: String
  let size: Int
}
struct SkillVersion: Codable, Hashable {
  let version: String
  let digest: String
  let name: String
  let description: String
  let body: String
  let resources: [SkillResource]
  let localPath: String
  let quarantined: Bool
}
struct SkillRow: Decodable, Identifiable {
  var id: String { source + ":" + package }
  let source: String
  let package: String
  let version: String
  let enabled: Bool
  let origin: String
  let blocked: Bool
  let scopeMode: String
  let selected: SkillVersion?
  let versions: [SkillVersion]
}
struct SkillInstall: Decodable {
  let source: String
  let package: String
  let version: String
  let digest: String
}
struct SkillScope: Identifiable {
  let scope: String
  let scopeID: String
  var id: String { scope + scopeID }
}

struct CompactionRecord: Decodable, Identifiable {
  let id: Int64
  let sourceHash: String
  let strategy: String
  let sourceCount: Int
  let beforeBytes: Int
  let afterBytes: Int
  let measureKind: String
  let status: String
  let error: String
  let created: Int64
}

struct ModelProbe: Decodable {
  let ok: Bool
  let httpStatus: Int
  let listKnown: Bool
  let modelFound: Bool
  let contextLimit: Int
  let outputLimit: Int
  let source: String
  let error: String?
}

struct HistoryIndexStatus: Decodable {
  let paused: Bool
  let available: Bool
  let bytes: Int64
  let quota: Int64
  let documents: Int64
  let pending: Int64
  let prepared: Int64
  let partial: Int64
  let fault: Int
}
