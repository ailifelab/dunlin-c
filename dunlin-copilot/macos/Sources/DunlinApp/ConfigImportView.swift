import SwiftUI

struct ConfigImportView: View {
  @EnvironmentObject var store: WorkspaceStore
  let plan: ConfigImportPlan
  @State private var error = ""
  private func protocolLabel(_ value: String) -> String {
    switch value {
    case "chat": return "Chat Completions"
    case "responses": return "Responses"
    case "anthropic": return "Anthropic Messages"
    case "ollama": return "Ollama"
    case "gemini": return "Gemini"
    default: return value
    }
  }
  private func mapping(_ source: String, _ destination: String) -> some View {
    HStack(alignment: .firstTextBaseline, spacing: 18) {
      Text(source).font(.callout.monospaced())
        .frame(maxWidth: .infinity, alignment: .leading)
      Text(destination).font(.callout)
        .frame(maxWidth: .infinity, alignment: .leading)
    }
    .accessibilityElement(children: .combine)
  }
  var body: some View {
    VStack(alignment: .leading, spacing: 20) {
      Label("静态检查通过", systemImage: "checkmark.circle.fill")
        .font(.callout.weight(.medium)).foregroundStyle(store.themeTint)
        .padding(.horizontal, 12).padding(.vertical, 7)
        .background(store.themeTint.opacity(0.10), in: Capsule())
      Text("字段映射 · 原配置 → Dunlin C").font(.headline)
      VStack(alignment: .leading, spacing: 12) {
        ForEach(plan.providers) { provider in
          let source = plan.sourceProviders?[provider.id]
          mapping(source?.path ?? "服务商", "新服务商：" + provider.name + " · " + (provider.routeID ?? provider.id))
          if let source {
            mapping(source.api, "协议：" + protocolLabel(provider.protocolName))
          }
          mapping("连接地址", provider.endpoint)
          if let reference = provider.keyEnvironment {
            mapping("apiKeyEnv = " + reference, "不读取环境变量，稍后关联钥匙串")
          }
          ForEach(Array(plan.models.filter { $0.providerID == provider.id }.enumerated()), id: \.element.id) { index, model in
            mapping(source == nil ? model.model : "models[\(index)].id = \(model.model)", "新模型：" + model.name)
          }
        }
        ForEach(plan.models.filter { $0.providerID == nil }) { model in
          mapping(model.model, "新模型：" + model.name)
          mapping(protocolLabel(model.protocolName), model.endpoint)
        }
        if !plan.warnings.isEmpty {
          VStack(alignment: .leading, spacing: 6) {
            Text("导入说明").font(.headline)
            ForEach(Array(plan.warnings.enumerated()), id: \.offset) { _, warning in
              Text(configImportWarning(warning)).font(.callout).foregroundStyle(.secondary)
            }
          }
        }
      }.padding(16).frame(maxWidth: .infinity, alignment: .leading)
        .background(store.themeSurface, in: RoundedRectangle(cornerRadius: 10))
      VStack(alignment: .leading, spacing: 6) {
        Text("导入后仍需完成连接设置").font(.headline)
        Text("已有聊天与默认模型保留原配置。导入后可关联凭据并单独测试新模型。").font(.callout).foregroundStyle(.secondary)
      }.padding(14).frame(maxWidth: .infinity, alignment: .leading)
        .background(Color.accentColor.opacity(0.08), in: RoundedRectangle(cornerRadius: 10))
      if !error.isEmpty {
        Text(error).foregroundStyle(.red).accessibilityIdentifier("config.import.error")
      }
      HStack {
        Spacer()
        Button("返回检查") { store.configImportPlan = nil }.keyboardShortcut(.cancelAction).accessibilityIdentifier("config.import.back")
        Button(store.configSaving ? "正在应用…" : "确认导入") {
          store.applyConfiguration(plan) { message in
            if let message { error = message }
          }
        }.buttonStyle(.borderedProminent).disabled(store.configSaving).accessibilityIdentifier(
          "config.import.confirm")
      }
    }.frame(maxWidth: .infinity, alignment: .topLeading)
  }
}
