import AppKit
import SwiftUI

struct ProviderSettingsView: View {
  @EnvironmentObject var store: WorkspaceStore
  @Binding var editing: ProviderProfile?
  var body: some View {
    if let provider = editing {
      ProviderEditor(provider: provider) { editing = nil }.id(provider.id)
    } else {
      VStack(alignment: .leading, spacing: 20) {
        HStack {
          Text("已配置服务商").font(.headline)
          Spacer()
          Button {
            var provider = ProviderProfile()
            provider.routeID = "provider-" + provider.id.prefix(8).lowercased()
            editing = provider
          } label: {
            Label("添加服务商…", systemImage: "plus")
          }
          .buttonStyle(.borderedProminent).accessibilityIdentifier("provider.add")
        }
        if store.providers.isEmpty {
          ContentUnavailableView(
            "添加一个服务商", systemImage: "network",
            description: Text("统一管理连接地址与凭据，再为它配置多个模型。已有独立模型可以继续使用。"))
        } else {
          VStack(spacing: 16) {
            ForEach(store.providers) { provider in
              Button {
                editing = provider
              } label: {
                HStack(spacing: 14) {
                  Image(systemName: "network").font(.title2).foregroundStyle(.secondary)
                  VStack(alignment: .leading, spacing: 5) {
                    Text(provider.name).fontWeight(.medium)
                    Text(provider.endpoint).font(.caption).foregroundStyle(.secondary)
                    Text("路由 \(provider.routeID ?? provider.id) · \(provider.protocolName)").font(
                      .caption2
                    ).foregroundStyle(.tertiary)
                    if let test = store.connectionTests.first(where: {
                      $0.providerID == provider.id
                    }) {
                      Label(
                        test.message,
                        systemImage: test.status == "passed"
                          ? "checkmark.circle" : "exclamationmark.circle"
                      )
                      .font(.caption).foregroundStyle(
                        test.status == "passed" ? Color.green : Color.secondary)
                    } else {
                      Text("尚未测试").font(.caption).foregroundStyle(.secondary)
                    }
                  }
                  Spacer()
                  Text("\(store.profiles.filter{$0.providerID==provider.id}.count) 个模型").font(
                    .callout
                  ).foregroundStyle(.secondary)
                  Image(systemName: "chevron.right").font(.caption).foregroundStyle(.tertiary)
                }.padding(16).contentShape(Rectangle())
              }.buttonStyle(.plain).accessibilityIdentifier("provider.edit.\(provider.id)")
              .background(store.themeSurface,in:RoundedRectangle(cornerRadius:12))
            }
          }
        }
        Text("连接与密钥变更在下一回合生效。关联模型仍有未完成回合时，需先处理恢复状态。").font(.caption).foregroundStyle(.secondary)
        HStack {Button("导入配置…",action:store.importModel);Button("管理模型") { store.settingsSection = "模型" }}
      }.disabled(!store.configurationReady || store.configSaving)
    }
  }
}

struct ProviderEditor: View {
  @EnvironmentObject var store: WorkspaceStore
  let onClose: () -> Void
  @State private var draft: ProviderProfile
  private let original: ProviderProfile
  @State private var secret = ""
  @State private var credentialIssue: ModelCredentialError?
  @State private var originalSecret: String?
  @State private var loaded = false
  @State private var error = ""
  @State private var ownerEpoch: Int64?
  @State private var testing = false
  @State private var cancellation: Callbacks?
  @State private var testResult: ProviderConnectionTest?
  @State private var testedIdentity: ProviderTestIdentity?
  @State private var plannedCredentialID: String?
  init(provider: ProviderProfile, onClose: @escaping () -> Void) {
    original = provider
    self.onClose = onClose
    _draft = State(initialValue: provider)
  }
  private var dependents: [ModelProfile] { store.profiles.filter { $0.providerID == draft.id } }
  var body: some View {
    VStack(alignment: .leading, spacing: 20) {
      VStack(alignment: .leading, spacing: 24) {
        HStack(alignment: .top, spacing: 24) {
          field("显示名称") {
            TextField("服务商名称", text: $draft.name).accessibilityIdentifier("provider.name")
          }
          field("路由 ID") {
            TextField(
              "local",
              text: Binding(get: { draft.routeID ?? draft.id }, set: { draft.routeID = $0 })
            ).accessibilityIdentifier("provider.routeID")
          }
        }
        HStack(alignment: .top, spacing: 24) {
          field("协议") {
            Picker("协议", selection: $draft.protocolName) {
              Text("Chat Completions").tag("chat")
              Text("Responses").tag("responses")
              Text("Anthropic Messages").tag("anthropic")
              Text("Ollama").tag("ollama")
              Text("Gemini").tag("gemini")
            }.labelsHidden().accessibilityIdentifier("provider.protocol")
          }
          field("Base URL") {
            TextField("API 根地址", text: $draft.endpoint).accessibilityIdentifier("provider.endpoint")
          }
        }
        field("认证方式") {
          Picker(
            "认证方式",
            selection: Binding(
              get: { draft.authentication ?? "auto" }, set: { draft.authentication = $0 })
          ) {
            Text("按协议选择").tag("auto")
            Text("Bearer Token").tag("bearer")
            Text("无需认证").tag("none")
          }.labelsHidden().accessibilityIdentifier("provider.authentication")
        }
        if draft.authentication != "none" {
          field("API 密钥") {
            SecureField("API 密钥", text: $secret).disabled(!loaded).accessibilityIdentifier(
              "provider.key")
          }
          if let issue = credentialIssue {
            HStack(spacing: 14) {
              VStack(alignment: .leading, spacing: 5) {
                Label(issue.title, systemImage: "lock.trianglebadge.exclamationmark").font(.headline)
                  .accessibilityIdentifier("provider.key.issue")
                Text(issue.localizedDescription).font(.callout)
              }
              Spacer()
              Button("重新授权", action: loadSecret).accessibilityIdentifier("provider.key.retry")
            }
            .padding(16).background(Color.orange.opacity(0.10), in: RoundedRectangle(cornerRadius: 10))
          }
        }
      }.textFieldStyle(.roundedBorder).controlSize(.large).disabled(testing)
      Text(
        draft.protocolName == "ollama"
          ? "Ollama 原生 API 根地址通常以 /api 结尾。" : "使用 API Key 时，明文仅存入 macOS 钥匙串；配置保留引用。"
      )
      .font(.caption).foregroundStyle(.secondary)
      if secretChanged {
        HStack(spacing:14) {
          VStack(alignment:.leading,spacing:5) {
            Label("凭据已更改",systemImage:"key").font(.headline)
            Text("保存后从下一次请求开始使用新凭据。").font(.callout)
          }
          Spacer()
          Button("撤销更改"){secret=originalSecret ?? ""}
        }.padding(16).background(Color.orange.opacity(0.10),in:RoundedRectangle(cornerRadius:10))
      }
      if let reference = draft.keyEnvironment {
        Text("导入的凭据引用：\(reference)。请单独填写密钥，不会读取环境变量。").font(.caption).foregroundStyle(.secondary)
      }
      GroupBox {
        VStack(alignment: .leading, spacing: 16) {
          Text("测试连接").font(.headline)
          HStack(spacing: 16) {
            Text("测试模型 ID").foregroundStyle(.secondary)
            TextField(
              "模型 ID",
              text: Binding(
                get: { draft.testModel ?? "" }, set: { draft.testModel = $0.isEmpty ? nil : $0 })
            )
            .textFieldStyle(.roundedBorder).disabled(testing).accessibilityIdentifier(
              "provider.testModel")
            if testing {
              ProgressView().controlSize(.small)
              Button("取消测试") { cancellation?.cancel() }.accessibilityIdentifier(
                "provider.testCancel")
            } else {
              Button("发送测试请求", action: test)
                .disabled(
                  !loaded || store.busy || store.checkingConnection
                    || (draft.testModel ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
                      .isEmpty
                )
                .accessibilityIdentifier("provider.test")
            }
          }
        }.padding(12)
      }
      VStack(alignment: .leading, spacing: 10) {
        if let result = visibleTest {
          Label(
            result.message,
            systemImage: result.status == "passed"
              ? "checkmark.circle.fill" : "exclamationmark.circle"
          )
          .foregroundStyle(result.status == "passed" ? Color.green : Color.secondary)
          .accessibilityIdentifier("provider.testResult")
          HStack(spacing: 18) {
            Text(Date(timeIntervalSince1970: Double(result.started)), style: .date)
            Text(Date(timeIntervalSince1970: Double(result.started)), style: .time)
            if let ms = result.durationMs { Text("请求耗时 \(ms) ms") }
          }.font(.caption).foregroundStyle(.secondary)
        } else {
          Label(testing ? "正在等待模型回复…" : "尚未测试", systemImage: "circle.fill").foregroundStyle(
            .secondary)
        }
        Text("发送一条最小测试消息可能产生服务商费用。结果仅覆盖所选协议与模型。").font(.caption).foregroundStyle(.secondary)
      }
      if !dependents.isEmpty {
        DisclosureGroup("关联模型（\(dependents.count)）") {
          if dependents.isEmpty { Text("尚未关联模型，可在模型设置中选择这个服务商。").foregroundStyle(.secondary) }
          ForEach(dependents) { model in LabeledContent(model.name, value: model.model) }
        }
      }
      if !error.isEmpty {
        Text(error).font(.callout).foregroundStyle(.red).accessibilityIdentifier("provider.error")
      }
      HStack {
        if store.providers.contains(where: { $0.id == draft.id }) {
          Button("删除服务商…", role: .destructive, action: remove).disabled(!dependents.isEmpty)
        }
        Spacer()
        Button("取消") {
          cancellation?.cancel()
          onClose()
        }
        Button("保存", action: save).buttonStyle(.borderedProminent).accessibilityIdentifier(
          "provider.save")
      }.disabled(testing)
    }.disabled(store.configSaving).onAppear(
      perform: {
        ownerEpoch = store.identityEpoch
        loadSecret()
      })
      .onChange(of: store.identityEpoch) { _, _ in
        cancellation?.cancel()
        onClose()
      }
      .onChange(of: draft.authentication) { _, _ in loadSecret() }
      .onChange(of: secret) { _, _ in
        testResult = nil
        testedIdentity = nil
        plannedCredentialID = nil
      }
      .onDisappear { cancellation?.cancel() }
  }
  private func field<Content: View>(_ title: String, @ViewBuilder content: () -> Content)
    -> some View
  {
    VStack(alignment: .leading, spacing: 8) {
      Text(title).font(.callout).foregroundStyle(.secondary)
      content().frame(maxWidth: .infinity, alignment: .leading)
    }
    .frame(maxWidth: .infinity, alignment: .leading)
  }
  private var secretChanged: Bool {
    loaded && draft.authentication != "none" && secret != (originalSecret ?? "")
  }
  private var candidate: ProviderProfile {
    var value = draft
    if secretChanged, let plannedCredentialID {
      value.credentialID = plannedCredentialID
      value.credentialRevision = (original.credentialRevision ?? 0) + 1
    }
    return value
  }
  private var visibleTest: ProviderConnectionTest? {
    if let testResult, testedIdentity == candidate.testIdentity { return testResult }
    if !secretChanged && draft.testIdentity == original.testIdentity {
      return store.connectionTests.first { $0.providerID == original.id }
    }
    return nil
  }
  private func test() {
    guard ownerEpoch == store.identityEpoch, !store.busy, !testing, !store.checkingConnection,
      !store.configSaving, loaded, (original.credentialRevision ?? 0) < Int.max
    else { return }
    if secretChanged && plannedCredentialID == nil { plannedCredentialID = UUID().uuidString }
    let value = candidate
    let box = Callbacks()
    cancellation = box
    testing = true
    error = ""
    testResult = nil
    testedIdentity = nil
    store.testProvider(value, secret: value.authentication == "none" ? "" : secret, cancel: box) {
      result, message in
      testing = false
      cancellation = nil
      guard ownerEpoch == store.identityEpoch else { return }
      testResult = result
      testedIdentity = value.testIdentity
      error = message ?? ""
    }
  }
  private func loadSecret() {
    if draft.authentication == "none" {
      secret = ""
      originalSecret = nil
      credentialIssue = nil
      loaded = true
      return
    }
    switch readCredential(draft.keychainID) {
    case .success(let value):
      credentialIssue = nil
      originalSecret = value
      secret = value ?? ""
      loaded = true
      error = ""
    case .failure(let failure):
      originalSecret = nil
      secret = ""
      credentialIssue = failure
      loaded = false
    }
  }
  private func save() {
    guard ownerEpoch == store.identityEpoch, !store.busy, !store.configSaving,
      !store.checkingConnection
    else {
      error = "身份已变化或配置正在使用，请重新打开后操作。"
      return
    }
    guard !draft.name.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else {
      error = "请填写服务商名称。"
      return
    }
    let changed = secretChanged
    var comparable = draft
    comparable.name = original.name
    comparable.routeID = original.routeID
    comparable.testModel = original.testModel
    let active = Set(store.chats.filter { $0.active }.map(\.model))
    if dependents.contains(where: { active.contains($0.id) }) && (comparable != original || changed)
    {
      error = "关联模型还有未完成回合，请先恢复并结束。"
      return
    }
    var value = draft
    var staged: String?
    if changed {
      guard (value.credentialRevision ?? 0) < Int.max else {
        error = "凭据版本不可用。"
        return
      }
      let id = plannedCredentialID ?? UUID().uuidString
      guard saveCredential(id, secret) else {
        error = "密钥未能保存到钥匙串。"
        return
      }
      staged = id
      value.credentialID = id
      value.credentialRevision = (value.credentialRevision ?? 0) + 1
    }
    var providers = store.providers
    if let i = providers.firstIndex(where: { $0.id == value.id }) {
      providers[i] = value
    } else {
      providers.append(value)
    }
    let models = store.profiles.map { $0.providerID == value.id ? value.applying(to: $0) : $0 }
    store.saveConfiguration(models: models, providers: providers) { ok, message in
      if ok {
        onClose()
      } else {
        error = message
        if let staged, !saveCredential(staged, "") { error += " 暂存凭据未能清理，请在钥匙串中核对。" }
      }
    }
  }
  private func remove() {
    guard ownerEpoch == store.identityEpoch, !store.busy, !store.configSaving,
      !store.checkingConnection
    else {
      error = "身份已变化或配置正在使用，请重新打开后操作。"
      return
    }
    guard dependents.isEmpty else {
      error = "请先移除或更换所有关联模型。"
      return
    }
    let alert = NSAlert()
    alert.messageText = "删除这个服务商配置？"
    alert.informativeText = "不会删除聊天记录。"
    alert.addButton(withTitle: "删除")
    alert.addButton(withTitle: "取消")
    alert.buttons[0].keyEquivalent = ""
    alert.window.defaultButtonCell = nil
    guard alert.runModal() == .alertFirstButtonReturn else { return }
    store.saveConfiguration(
      models: store.profiles, providers: store.providers.filter { $0.id != draft.id }
    ) { ok, message in
      if ok {
        if !saveCredential(draft.keychainID, "") { store.error = "服务商已删除，钥匙串凭据未能移除，请手动核对。" }
        onClose()
      } else {
        error = message
      }
    }
  }
}
