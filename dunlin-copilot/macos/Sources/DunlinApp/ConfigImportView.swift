import SwiftUI

struct ConfigImportView: View {
  @EnvironmentObject var store: WorkspaceStore
  @Environment(\.dismiss) private var dismiss
  let plan: ConfigImportPlan
  @State private var error = ""
  var body: some View {
    VStack(alignment: .leading, spacing: 20) {
      Text("确认导入配置").font(.title2.weight(.semibold))
      Text(
        "将新增 \(plan.providers.count) 个服务商和 \(plan.models.count) 个模型。现有路由与聊天保留。预览在 \(plan.lifetimeSeconds / 60) 分钟内有效。"
      ).foregroundStyle(.secondary)
      List {
        if !plan.providers.isEmpty {
          Section("新服务商") {
            ForEach(plan.providers) { provider in
              VStack(alignment: .leading, spacing: 5) {
                HStack {
                  Text(provider.name).fontWeight(.medium)
                  Spacer()
                  Text(provider.routeID ?? provider.id).font(.caption.monospaced()).foregroundStyle(
                    .secondary)
                }
                Text(provider.endpoint).font(.caption).foregroundStyle(.secondary)
                if let reference = provider.keyEnvironment {
                  Text("凭据引用：\(reference) · 稍后关联钥匙串").font(.caption).foregroundStyle(.secondary)
                }
              }.padding(.vertical, 5)
            }
          }
        }
        Section("新模型") {
          ForEach(plan.models) { model in
            VStack(alignment: .leading, spacing: 5) {
              HStack {
                Text(model.name).fontWeight(.medium)
                Spacer()
                Text(model.protocolName).foregroundStyle(.secondary)
              }
              Text(model.model).font(.callout)
              Text(model.endpoint).font(.caption).foregroundStyle(.secondary)
            }.padding(.vertical, 5)
          }
        }
        if !plan.warnings.isEmpty {
          Section("导入说明") {
            ForEach(Array(plan.warnings.enumerated()), id: \.offset) { _, warning in
              Text(configImportWarning(warning)).font(.callout).foregroundStyle(.secondary)
            }
          }
        }
      }.listStyle(.inset)
      Text("文件中的密钥值不会导入，也不会在预览期间连接服务。导入后请填写密钥并单独测试连接。").font(.caption).foregroundStyle(.secondary)
      if !error.isEmpty {
        Text(error).foregroundStyle(.red).accessibilityIdentifier("config.import.error")
      }
      HStack {
        Spacer()
        Button("取消") { dismiss() }
        Button(store.configSaving ? "正在应用…" : "确认导入") {
          store.applyConfiguration(plan) { message in
            if let message { error = message } else { dismiss() }
          }
        }.buttonStyle(.borderedProminent).disabled(store.configSaving).accessibilityIdentifier(
          "config.import.confirm")
      }
    }.padding(28).frame(width: 720, height: 620)
  }
}
