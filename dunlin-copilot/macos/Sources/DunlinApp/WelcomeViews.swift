import SwiftUI

struct FirstLaunchView: View {
  @EnvironmentObject var store: WorkspaceStore
  @Environment(\.openSettings) private var openSettings
  var body: some View {
    VStack(spacing: 0) {
      Spacer(minLength: 30)
      Text("Dunlin Copilot").font(.system(size: 44, weight: .semibold))
      Text("让想法在你的 Mac 上继续。").font(.system(size: 22)).foregroundStyle(.secondary).padding(.top, 18)
      HStack(spacing: 38) {
        step("连接模型", lines: ["使用自己的 API 服务", "或连接本地模型"], number: "01")
        step("准备一个项目", lines: ["关联本地文件夹", "让相关聊天放在一起"], number: "02")
        step("从一次对话开始", lines: ["保留聊天与项目上下文", "随时回来继续"], number: "03")
      }.padding(.top, 52).padding(.bottom, 40)
      Button("配置第一个服务商") {
        store.settingsSection = "服务商"
        openSettings()
      }
      .buttonStyle(.borderedProminent).controlSize(.large).accessibilityIdentifier(
        "onboarding.provider")
      Button("先浏览项目", action: store.browseProjects).buttonStyle(.plain).padding(.top, 22)
        .accessibilityIdentifier("onboarding.projects")
      Spacer(minLength: 40)
      Text("模型请求发往你配置的服务商；聊天保存在本机。")
        .font(.system(size: 13)).foregroundStyle(.secondary).padding(.bottom, 36)
    }.frame(maxWidth: 840).padding(.horizontal, 36).frame(maxWidth: .infinity, maxHeight: .infinity)
  }
  private func step(_ title: String, lines: [String], number: String) -> some View {
    VStack(alignment: .leading, spacing: 16) {
      Text(title).font(.system(size: 18, weight: .semibold))
      VStack(alignment: .leading, spacing: 8) {
        ForEach(lines, id: \.self) { Text($0).font(.system(size: 14)).foregroundStyle(.secondary) }
      }
      Spacer(minLength: 14)
      Text(number).font(.system(size: 12, weight: .medium)).foregroundStyle(Color.accentColor)
        .padding(.horizontal, 12).padding(.vertical, 5)
        .background(Color.accentColor.opacity(0.07), in: Capsule())
    }.padding(22).frame(width: 250, height: 188, alignment: .topLeading)
      .background(Color.primary.opacity(0.025), in: RoundedRectangle(cornerRadius: 14))
      .overlay(RoundedRectangle(cornerRadius: 14).strokeBorder(Color.primary.opacity(0.07)))
  }
}
struct NewConversationView: View {
  @EnvironmentObject var store: WorkspaceStore
  var body: some View {
    VStack(spacing: 24) {
      Spacer(minLength: 24)
      Text("今天想一起做什么？").font(.system(size: 32, weight: .semibold))
      Text("从一个问题开始，或者打开你的项目。").font(.system(size: 16)).foregroundStyle(.secondary)
      HStack(spacing: 22) {
        suggestion("把想法讲清楚", detail: "梳理思路，写一份清楚的说明", draft: "帮我梳理思路，写一份清楚的说明：")
        suggestion("理解一段内容", detail: "添加文本或一张 PNG 图片", draft: "帮我理解以下内容，提取关键点：")
      }.padding(.top, 24)
      Spacer(minLength: 30)
    }.frame(maxWidth: 800).padding(.horizontal, 36).frame(maxWidth: .infinity, maxHeight: .infinity)
  }
  private func suggestion(_ title: String, detail: String, draft: String) -> some View {
    Button {
      store.draft = draft
    } label: {
      VStack(alignment: .leading, spacing: 14) {
        Text(title).font(.system(size: 18, weight: .semibold))
        Text(detail).font(.system(size: 14)).foregroundStyle(.secondary)
      }.padding(22).frame(maxWidth: .infinity, minHeight: 128, alignment: .leading)
        .background(Color.primary.opacity(0.025), in: RoundedRectangle(cornerRadius: 14))
        .overlay(RoundedRectangle(cornerRadius: 14).strokeBorder(Color.primary.opacity(0.07)))
    }.buttonStyle(.plain).disabled(
      store.selected == nil || !store.draft.isEmpty || store.creatingChat)
  }
}
