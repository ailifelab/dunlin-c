import AppKit
import CDesktop
import SwiftUI

struct ProviderProfile: Codable, Identifiable, Hashable {
  var id = UUID().uuidString
  var name = "新服务商"
  var endpoint = "http://127.0.0.1:8080/v1"
  var protocolName = "chat"
  var authentication: String?
  var credentialRevision: Int?
  var credentialID: String?
  var routeID: String?
  var keyEnvironment: String?
  var testModel: String?
  var keychainID: String { credentialID ?? id }
  var testIdentity: ProviderTestIdentity {
    ProviderTestIdentity(
      id: id, endpoint: endpoint, protocolName: protocolName,
      model: testModel ?? "", authentication: authentication ?? "auto",
      credentialID: authentication == "none" ? "" : keychainID,
      credentialRevision: authentication == "none" ? 0 : (credentialRevision ?? 0))
  }
  func applying(to model: ModelProfile) -> ModelProfile {
    var value = model
    if value.endpoint != endpoint || value.protocolName != protocolName {
      value.contextLimit = nil
      value.contextSource = nil
      value.advertisedOutputLimit = nil
      value.contextBudget = nil
      value.deploymentLimit = nil
      value.outputLimit = nil
      value.inputModalities = nil
      value.reasoningSupported = nil
      value.reasoningCapabilities = nil
      value.reasoningEffort = nil
      value.temperature = nil
      value.enableThinking = nil
      value.chatTemplate = nil
    }
    value.providerID = id
    value.endpoint = endpoint
    value.protocolName = protocolName
    value.authentication = authentication
    value.credentialRevision = credentialRevision
    value.credentialID = credentialID
    value.keyEnvironment = nil
    return value
  }
}
struct ConfigRegistry: Decodable {
  let revision: Int64
  let models: [ModelProfile]
  let providers: [ProviderProfile]
  let connectionTests: [ProviderConnectionTest]?
  let modelIssues: [ModelConfigIssue]?
}
struct ModelConfigIssue: Decodable, Identifiable {
  let id: String
  let error: String
}
struct ProviderTestIdentity: Equatable {
  let id: String, endpoint: String, protocolName: String, model: String, authentication: String
  let credentialID: String
  let credentialRevision: Int
}
struct ProviderConnectionTest: Decodable, Identifiable {
  let id: String, providerID: String, fingerprint: String, model: String, status: String
  let started: Int64
  let finished: Int64?
  let httpStatus: Int?
  let modelStatus: Int?
  let durationMs: Int64?
  let error: String?
  var message: String {
    let http = httpStatus.map { " · HTTP \($0)" } ?? ""
    switch status {
    case "passed": return "测试通过 · 已收到模型文本回复\(http)"
    case "cancelled": return "测试已取消\(http)"
    case "interrupted": return "上次测试中断，请重新发送测试请求。"
    case "running": return "测试尚未结算。"
    default:
      let reason: String
      switch error {
      case "test_http_error": reason = "服务返回错误，请检查地址、认证与模型 ID。"
      case "test_rate_limit": reason = "服务正在限流，请稍后重试。"
      case "test_timeout": reason = "测试请求超时。"
      case "test_protocol_error": reason = "返回格式与所选协议不一致。"
      case "test_transport_error": reason = "连接失败，请检查服务与网络。"
      case "test_response_limit": reason = "返回内容超过测试上限。"
      case "test_no_text": reason = "模型未返回可显示的文本。"
      case "test_unexpected_tool_call": reason = "模型返回工具调用；测试没有执行它。"
      default: reason = "模型测试未通过，请核对连接与模型 ID。"
      }
      return reason + http
    }
  }
}
struct ConfigImportPlan: Decodable, Identifiable {
  var id: String { plan }
  let plan: String
  let revision: Int64
  let models: [ModelProfile]
  let providers: [ProviderProfile]
  let warnings: [String]
  let lifetimeSeconds: Int
}
extension WorkspaceStore {
  func applyModelCapabilities(
    _ id: String, contextLimit: Int, outputLimit: Int, source: String,
    completion: @escaping (Bool, String) -> Void
  ) {
    guard configurationReady, !busy, !configSaving, !checkingConnection else {
      completion(false, "配置正在使用，请稍后重试。")
      return
    }
    configSaving = true
    workspaceRequest(
      [
        "op": "config.capabilities", "id": id, "revision": configRevision,
        "contextLimit": contextLimit, "outputLimit": outputLimit, "source": source,
      ], as: ConfigRegistry.self
    ) { reply in
      self.configSaving = false
      guard reply.ok, let value = reply.data else {
        completion(false, self.configurationError(reply.error))
        return
      }
      self.acceptConfiguration(value)
      self.saveSettings()
      completion(true, "")
    }
  }
  nonisolated func coreModelSettings(_ h: NativeStoreReference, id: String, revision: Int64)
    -> (data: [String: Any]?, error: String?)
  {
    guard
      let bytes = try? JSONSerialization.data(withJSONObject: [
        "op": "config.model", "id": id, "revision": revision,
      ]),
      let request = String(data: bytes, encoding: .utf8),
      let pointer = dc_workspace_request(h.raw, request)
    else { return (nil, "configuration_storage_error") }
    defer { dc_free(pointer) }
    guard
      let reply = (try? JSONSerialization.jsonObject(with: Data(String(cString: pointer).utf8)))
        as? [String: Any],
      reply["ok"] as? Bool == true, let data = reply["data"] as? [String: Any]
    else {
      let reply =
        (try? JSONSerialization.jsonObject(with: Data(String(cString: pointer).utf8)))
        as? [String: Any]
      return (nil, reply?["error"] as? String ?? "configuration_storage_error")
    }
    return (data, nil)
  }
  nonisolated func loadConfiguration(_ h:NativeStoreReference)->ConfigRegistry? {
    let reply=decode(dc_workspace_request(h.raw,"{\"op\":\"config.list\"}"),as:WorkspaceReply<ConfigRegistry>.self)
    return reply?.ok == true ? reply?.data:nil
  }
  func acceptConfiguration(_ value: ConfigRegistry) {
    profiles = value.models
    providers = value.providers
    connectionTests = value.connectionTests ?? []
    modelIssues = value.modelIssues ?? []
    configRevision = value.revision
    configurationReady = true
    if !profiles.contains(where: { $0.id == profileID }) { profileID = profiles.first?.id ?? "" }
    if !profiles.contains(where: { $0.id == defaultProfileID }) {
      defaultProfileID = profiles.first?.id ?? ""
    }
  }
  func saveConfiguration(
    models: [ModelProfile], providers: [ProviderProfile],
    completion: @escaping (Bool, String) -> Void
  ) {
    guard configurationReady, !configSaving, !busy, !creatingChat, !switchingModel,
      !checkingConnection, let h = handle
    else {
      completion(false, "配置正在使用或尚未就绪，请稍后重试。")
      return
    }
    do {
      let m = try JSONSerialization.jsonObject(with: JSONEncoder().encode(models))
      let p = try JSONSerialization.jsonObject(with: JSONEncoder().encode(providers))
      let bytes = try JSONSerialization.data(withJSONObject: [
        "op": "config.save", "revision": configRevision, "models": m, "providers": p,
      ])
      guard let raw = String(data: bytes, encoding: .utf8) else {
        completion(false, "配置编码失败。")
        return
      }
      configSaving = true
      let epoch = identityEpoch
      queue.async {
        let reply = self.decode(
          dc_workspace_request(h.raw, raw), as: WorkspaceReply<ConfigRegistry>.self)
        DispatchQueue.main.async {
          guard self.identityEpoch == epoch else {
            completion(false, "身份已变化，配置未应用到当前身份。")
            return
          }
          self.configSaving = false
          guard reply?.ok == true, let value = reply?.data else {
            completion(false, self.configurationError(reply?.error))
            return
          }
          self.acceptConfiguration(value)
          self.saveSettings()
          completion(true, "")
        }
      }
    } catch { completion(false, error.localizedDescription) }
  }
  func testProvider(
    _ provider: ProviderProfile, secret: String, cancel: Callbacks,
    completion: @escaping (ProviderConnectionTest?, String?) -> Void
  ) {
    guard !busy, !checkingConnection, !configSaving, configurationReady, let h = handle,
      secret.utf8.count <= 16384, !secret.contains("\r"), !secret.contains("\n"),
      !secret.utf8.contains(0),
      let bytes = try? JSONEncoder().encode(provider),
      let raw = String(data: bytes, encoding: .utf8)
    else {
      completion(nil, "配置正在使用或测试参数无效，请稍后重试。")
      return
    }
    let epoch = identityEpoch
    checkingConnection = true
    queue.async {
      var hooks = dc_hooks(
        user: Unmanaged.passUnretained(cancel).toOpaque(), event: nil, approve: nil,
        cancelled: { context in
          guard let context else { return 1 }
          return Unmanaged<Callbacks>.fromOpaque(context).takeUnretainedValue().isCancelled()
            ? 1 : 0
        })
      let result = self.decode(
        dc_test_connection(h.raw, raw, secret, &hooks),
        as: WorkspaceReply<ProviderConnectionTest>.self)
      let registry = self.decode(
        dc_workspace_request(h.raw, "{\"op\":\"config.list\"}"),
        as: WorkspaceReply<ConfigRegistry>.self)
      DispatchQueue.main.async {
        guard self.identityEpoch == epoch else { return }
        self.checkingConnection = false
        if registry?.ok == true { self.connectionTests = registry?.data?.connectionTests ?? [] }
        if result?.ok == true, let data = result?.data {
          completion(data, nil)
        } else {
          completion(
            nil,
            result?.error == "test_result_storage_error"
              ? "请求已经结束，但测试结果未能保存。请重新测试。"
              : result?.error == "test_configuration_invalid"
                ? "测试参数无效，请检查服务地址、协议和模型 ID。"
                : "测试未能开始或本地记录不可用，请稍后重试。")
        }
      }
    }
  }
  func configurationError(_ value: String?, diagnostic: ConfigDiagnostic? = nil) -> String {
    if let diagnostic { return configDiagnosticMessage(diagnostic) }
    switch value {
    case "configuration_active_route": return "关联模型还有未完成回合。请先恢复并结束，再修改连接或凭据。"
    case "configuration_invalid": return "配置包含无效或不支持的模型参数，请核对协议、能力与预算。"
    case "configuration_model_missing": return "模型配置已移除，请重新选择模型。"
    case "configuration_model_options_unsupported": return "这个模型含有与协议不兼容的旧参数，请在模型设置中修复后保存。"
    case "configuration_capability_conflict": return "服务窗口无法容纳当前协议余量，请核对部署能力与预留后重新检查。"
    case "configuration_revision_conflict", "configuration_revision_or_storage_error":
      return "配置版本已变化或保存失败，请关闭后重新打开此页面。"
    case "configuration_unsupported_or_invalid":
      return "配置含不支持或无效的字段。仅支持静态 JSON／YAML；动态标签、脚本和 includes 不会执行。"
    case "configuration_source_invalid": return "配置文件无效，请选择有效的 UTF-8 静态 JSON／YAML。"
    case "configuration_plan_stale_or_storage_error": return "导入预览已过期、已使用或当前配置发生变化，请重新选择文件预览。"
    default: return "配置未应用：\(value ?? "本地服务暂时不可用")"
    }
  }
  func previewConfiguration(_ source: String) {
    guard !busy, !configSaving, configurationReady else {
      error = "配置服务尚未就绪。"
      return
    }
    workspaceRequest(["op": "config.preview", "source": source], as: ConfigImportPlan.self) {
      result in
      if result.ok, let plan = result.data {
        self.configImportPlan = plan
      } else {
        self.connectionStatus = self.configurationError(result.error, diagnostic: result.diagnostic)
        self.error = self.connectionStatus
      }
    }
  }
  func applyConfiguration(_ plan: ConfigImportPlan, completion: @escaping (String?) -> Void) {
    guard !busy, !configSaving, configurationReady else {
      completion("配置正在使用，请稍后重试。")
      return
    }
    configSaving = true
    workspaceRequest(
      ["op": "config.apply", "plan": plan.plan, "confirmed": true], as: ConfigRegistry.self
    ) { result in
      self.configSaving = false
      if result.ok, let value = result.data {
        self.acceptConfiguration(value)
        self.saveSettings()
        self.connectionStatus = "已导入 \(plan.models.count) 个新模型；凭据需单独填写，连接尚未测试。"
        completion(nil)
      } else {
        completion(self.configurationError(result.error))
      }
    }
  }
}
